// SPDX-License-Identifier: Apache-2.0 OR MIT
// Renesas (Adesto) AT45DB041E 4-Mbit DataFlash on SPI, in its "power of 2"
// binary page size, served as a command image: Continuous Array Read in
// low power mode (01h) is the image's memory read, Status Register Read
// (D7h) the status
// it serves, and every other command arrives as a window after chip select
// rises and is executed here. Section numbers are data sheet
// DS-AT45DB041E-8783 Rev. Q's.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/command.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#define OP_READ_LOW_POWER 0x01
#define OP_PAGE_PROGRAM 0x02
#define OP_MODIFY_1 0x58
#define OP_MODIFY_2 0x59
#define OP_PAGE_ERASE 0x81
#define OP_BLOCK_ERASE 0x50
#define OP_SECTOR_ERASE 0x7c
#define OP_CHIP_ERASE 0xc7
#define OP_STATUS 0xd7
#define OP_READ_ID 0x9f
#define OP_DEEP_POWER_DOWN 0xb9
#define OP_RESUME 0xab
#define OP_ULTRA_DEEP_POWER_DOWN 0x79
#define OP_PAGE_SIZE 0x3d

// 2.1 and Table 20: 2048 pages of 256 bytes, 4 Mbit; blocks of 8 pages,
// sector 0a of 8 pages and 0b of 248, sectors 1-7 of 256.
#define CAPACITY_BYTES (512u * 1024u)
#define PAGE_SIZE 256u
#define BLOCK_SIZE (8u * PAGE_SIZE)
#define SECTOR_SIZE (256u * PAGE_SIZE)
#define HELD_PAGES MCLW_COMMAND_PAGES_MAX
#define HELD_BYTES (HELD_PAGES * PAGE_SIZE)

// Table 20: RDY/BUSY (bit 7) ready, density 0111 (four Mbit), the binary
// page size (bit 0).
#define STATUS_READY 0x80u
#define STATUS_DENSITY (0x7u << 2)
#define STATUS_BINARY_PAGE 0x01u
#define STATUS (STATUS_READY | STATUS_DENSITY | STATUS_BINARY_PAGE)

// Table 23: manufacturer 1Fh, device 24h 00h; then the EDI string length,
// 01h, and one EDI byte, 00h.
static const uint8_t ID[] = { 0x1f, 0x24, 0x00, 0x01, 0x00 };

static const uint8_t BUSY_OPCODES[] = {
    OP_PAGE_PROGRAM, OP_MODIFY_1, OP_MODIFY_2, OP_PAGE_ERASE, OP_BLOCK_ERASE, OP_SECTOR_ERASE, OP_CHIP_ERASE,
};

enum {
    PROP_POWER_DOWN,
    PROP_READS,
    PROP_PROGRAMS,
    PROP_ERASES,
    PROP_LOST,
    PROP_IGNORED,
    PROP_SPI_MODE,
    PROP_HELD_BASE,
    PROP_LAST_IGNORED,
};

enum {
    BINDING_SPI,
};

enum ignore_reason {
    IGNORED_POWER_DOWN = 1,
    IGNORED_MALFORMED,
    IGNORED_UNKNOWN,
};

struct at45db_state {
    struct mclw_command_image image;
    struct mclw_spi_target *spi;
    bool power_down;
    uint32_t reads;
    uint32_t programs;
    uint32_t erases;
    uint32_t lost;
    uint32_t ignored;
    uint8_t last_ignored[2];
    // The address of the first held byte; the held pages follow it, named
    // by address bits 15..8 as the engine serves them.
    uint32_t held_base;
};

static const struct mclw_property_desc at45db_properties[] = {
    { .name = "power_down", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "reads", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "programs", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "erases", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "lost", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "ignored", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    MCLW_PROPERTY_SPI_MODE,
    { .name = "held_base", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = CAPACITY_BYTES - PAGE_SIZE },
    // The last command ignored: its opcode, then why: 1 in Deep
    // Power-Down, 2 a malformed frame, 3 unknown.
    { .name = "last_ignored", .type = MCLW_TYPE_U32, .count = 2, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
};

static const struct mclw_region_desc at45db_regions[] = {
    { .name = "array", .size = HELD_BYTES, .access = MCLW_ACCESS_RW },
};

// 4: SPI modes 0 and 3, MSB first.
static const struct mclw_binding_desc at45db_bindings[] = {
    {
        .name = "spi",
        .kind = MCLW_BINDING_SPI_TARGET,
        .required = true,
        .usb = NULL,
        .spi = {
            .modes = MCLW_SPI_MODE_BIT(0) | MCLW_SPI_MODE_BIT(3),
            .default_mode = 0,
            .msb_first = true,
        },
        .uart = { 0 },
    },
};

static uint32_t address_of(const uint8_t *bytes) {
    return (((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2]) & (CAPACITY_BYTES - 1u);
}

// A held byte, or NULL for one outside the held pages. The held pages may
// cross a 64 KiB boundary: the engine tells them apart by bits 15..8 alone.
static uint8_t *held(struct at45db_state *s, uint32_t address) {
    uint32_t offset = (address - s->held_base) & (CAPACITY_BYTES - 1u);

    if (offset >= HELD_BYTES) {
        return NULL;
    }
    return mclw_command_register(&s->image, (uint16_t)address);
}

static void publish(struct at45db_state *s) {
    mclw_command_set_status(&s->image, STATUS);
    mclw_command_publish(&s->image);
}

static void ignore(struct at45db_state *s, uint8_t opcode, enum ignore_reason reason) {
    s->ignored++;
    s->last_ignored[0] = opcode;
    s->last_ignored[1] = (uint8_t)reason;
}

// 7.5, Main Memory Byte/Page Program through Buffer 1 (02h): the bytes go
// into the addressed page from the given byte on, wrapping within the
// buffer, a later byte replacing an earlier one; only the bytes clocked in
// are programmed, into locations that should be erased (a program clears
// bits). 7.6, Read-Modify-Write (58h, 59h): the same bytes, but each
// location is erased first, so it takes the byte as given.
static void page_program(struct at45db_state *s, const uint8_t *bytes, size_t len, bool erase_first) {
    uint32_t address = address_of(bytes);
    uint32_t page = address & ~(PAGE_SIZE - 1u);
    uint8_t latch[PAGE_SIZE];
    bool clocked[PAGE_SIZE];

    memset(clocked, 0, sizeof(clocked));
    for (size_t i = 3; i < len; i++) {
        uint32_t at = (address + (uint32_t)(i - 3u)) & (PAGE_SIZE - 1u);

        latch[at] = bytes[i];
        clocked[at] = true;
    }
    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        uint8_t *cell;

        if (!clocked[i]) {
            continue;
        }
        cell = held(s, page | i);
        if (cell != NULL) {
            *cell = erase_first ? latch[i] : (uint8_t)(*cell & latch[i]);
        } else if (latch[i] != 0xffu) {
            s->lost++;
        }
    }
    s->programs++;
}

// 7.7 to 7.10: every byte of the aligned page, block, sector or chip reads
// FFh afterwards.
static void erase(struct at45db_state *s, uint32_t first, uint32_t size) {
    for (uint32_t at = 0; at < HELD_BYTES; at++) {
        uint32_t address = (s->held_base + at) & (CAPACITY_BYTES - 1u);

        if (address >= first && address < first + size) {
            *held(s, address) = 0xff;
        }
    }
    s->erases++;
}

// 7.9: sector 0a is the first block, 0b the rest of the first 64 KiB.
static void sector_erase(struct at45db_state *s, uint32_t address) {
    if (address < BLOCK_SIZE) {
        erase(s, 0, BLOCK_SIZE);
    } else if (address < SECTOR_SIZE) {
        erase(s, BLOCK_SIZE, SECTOR_SIZE - BLOCK_SIZE);
    } else {
        erase(s, address & ~(SECTOR_SIZE - 1u), SECTOR_SIZE);
    }
}

// Every operation is done when its window arrives; until then the engine
// serves RDY/BUSY clear (build_image). In Deep Power-Down every command but
// Resume is ignored (11).
static void at45db_on_window(struct mclw_command_image *image, const uint8_t *bytes, size_t len, bool busy,
    void *ctx) {
    struct at45db_state *s = ctx;
    uint8_t opcode = len != 0 ? bytes[0] : 0;

    (void)image;
    (void)busy;
    if (len == 0) {
        return;
    }
    if (s->power_down && opcode != OP_RESUME) {
        ignore(s, opcode, IGNORED_POWER_DOWN);
    } else {
        switch (opcode) {
            case OP_STATUS:
            case OP_READ_ID:
                break;
            case OP_READ_LOW_POWER:
                s->reads++;
                break;
            case OP_PAGE_PROGRAM:
            case OP_MODIFY_1:
            case OP_MODIFY_2:
                if (len >= 5) {
                    page_program(s, &bytes[1], len - 1, opcode != OP_PAGE_PROGRAM);
                } else {
                    ignore(s, opcode, IGNORED_MALFORMED);
                }
                break;
            case OP_PAGE_ERASE:
            case OP_BLOCK_ERASE:
            case OP_SECTOR_ERASE:
                if (len == 4) {
                    uint32_t address = address_of(&bytes[1]);

                    if (opcode == OP_PAGE_ERASE) {
                        erase(s, address & ~(PAGE_SIZE - 1u), PAGE_SIZE);
                    } else if (opcode == OP_BLOCK_ERASE) {
                        erase(s, address & ~(BLOCK_SIZE - 1u), BLOCK_SIZE);
                    } else {
                        sector_erase(s, address);
                    }
                } else {
                    ignore(s, opcode, IGNORED_MALFORMED);
                }
                break;
            // 7.10, Chip Erase: C7h 94h 80h 9Ah.
            case OP_CHIP_ERASE:
                if (len == 4 && bytes[1] == 0x94 && bytes[2] == 0x80 && bytes[3] == 0x9a) {
                    erase(s, 0, CAPACITY_BYTES);
                } else {
                    ignore(s, opcode, IGNORED_MALFORMED);
                }
                break;
            // 9.2, "Power of 2" Binary Page Size Option: 3Dh 2Ah 80h A6h;
            // the model is always in the binary page size.
            case OP_PAGE_SIZE:
                if (!(len == 4 && bytes[1] == 0x2a && bytes[2] == 0x80 && bytes[3] == 0xa6)) {
                    ignore(s, opcode, IGNORED_MALFORMED);
                }
                break;
            case OP_DEEP_POWER_DOWN:
            case OP_ULTRA_DEEP_POWER_DOWN:
                s->power_down = true;
                break;
            case OP_RESUME:
                s->power_down = false;
                break;
            default:
                ignore(s, opcode, IGNORED_UNKNOWN);
                break;
        }
    }
    mclw_trace(mclw_instance_from_state(s), MCLW_TRACE_COMMAND, opcode, (uint16_t)len, 0);
    publish(s);
}

static void build_image(struct at45db_state *s) {
    mclw_command_init(&s->image, OP_READ_LOW_POWER, OP_READ_LOW_POWER);
    mclw_command_set_memory_read(&s->image, OP_READ_LOW_POWER);
    mclw_command_set_filler(&s->image, 0xff);
    // RDY/BUSY reads busy from the release of a program or erase until the
    // model has done it, so the DUT, which polls it after each, never reads
    // the array ahead of the model.
    mclw_command_set_ready_status(&s->image, STATUS_READY);
    for (size_t i = 0; i < sizeof(BUSY_OPCODES); i++) {
        mclw_command_add_busy_opcode(&s->image, BUSY_OPCODES[i]);
    }
    for (uint8_t page = 0; page < HELD_PAGES; page++) {
        uint8_t number = (uint8_t)((s->held_base >> 8) + page);

        (void)mclw_command_add_page(&s->image, number);
        memset(mclw_command_register(&s->image, (uint16_t)(number << 8)), 0xff, PAGE_SIZE);
    }
    // 13: the ID bytes follow the opcode at once.
    mclw_command_set_immediate_response(&s->image);
    (void)mclw_command_add_response(&s->image, OP_READ_ID);
    (void)mclw_command_set_response(&s->image, OP_READ_ID, ID, sizeof(ID));
    mclw_command_set_on_window(&s->image, at45db_on_window, s);
}

static int rebuild(struct at45db_state *s) {
    mclw_hal_spi_target_disarm(s->spi);
    build_image(s);
    publish(s);
    return mclw_hal_spi_target_arm_command(s->spi, &s->image, NULL);
}

static int at45db_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct at45db_state *s = inst->state;

    s->spi = bindings[BINDING_SPI].spi;
    s->power_down = false;
    s->held_base = 0;
    build_image(s);
    publish(s);
    return mclw_hal_spi_target_arm_command(s->spi, &s->image, NULL);
}

static void at45db_deinit(struct mclw_instance *inst) {
    struct at45db_state *s = inst->state;

    mclw_hal_spi_target_disarm(s->spi);
    s->spi = NULL;
}

// A power cycle keeps the array, which is non-volatile; factory erases it.
static int at45db_reset(struct mclw_instance *inst, bool factory) {
    struct at45db_state *s = inst->state;

    s->power_down = false;
    if (factory) {
        for (uint32_t at = 0; at < HELD_BYTES; at++) {
            *held(s, (s->held_base + at) & (CAPACITY_BYTES - 1u)) = 0xff;
        }
    }
    s->reads = 0;
    s->programs = 0;
    s->erases = 0;
    s->lost = 0;
    s->ignored = 0;
    s->last_ignored[0] = 0;
    s->last_ignored[1] = 0;
    publish(s);
    return 0;
}

static int at45db_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct at45db_state *s = inst->state;

    switch (property) {
        case PROP_POWER_DOWN:
            out->v[0] = s->power_down;
            return 0;
        case PROP_READS:
            out->v[0] = s->reads;
            return 0;
        case PROP_PROGRAMS:
            out->v[0] = s->programs;
            return 0;
        case PROP_ERASES:
            out->v[0] = s->erases;
            return 0;
        case PROP_LOST:
            out->v[0] = s->lost;
            return 0;
        case PROP_IGNORED:
            out->v[0] = s->ignored;
            return 0;
        case PROP_SPI_MODE:
            return mclw_spi_mode_get(inst, BINDING_SPI, out);
        case PROP_HELD_BASE:
            out->v[0] = s->held_base;
            return 0;
        case PROP_LAST_IGNORED:
            out->v[0] = s->last_ignored[0];
            out->v[1] = s->last_ignored[1];
            return 0;
        default:
            return -EINVAL;
    }
}

// held_base is a page boundary; the held pages may run past a 64 KiB
// boundary, but not onto pages they already hold (bits 15..8 name them).
static int at45db_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct at45db_state *s = inst->state;

    switch (property) {
        case PROP_SPI_MODE:
            return mclw_spi_mode_set(inst, BINDING_SPI, in);
        case PROP_HELD_BASE: {
            uint32_t base = (uint32_t)in->v[0];

            if ((base & (PAGE_SIZE - 1u)) != 0 || base > CAPACITY_BYTES - HELD_BYTES) {
                return -EINVAL;
            }
            s->held_base = base;
            return rebuild(s);
        }
        default:
            return -EINVAL;
    }
}

static int at45db_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    struct at45db_state *s = inst->state;
    uint8_t *out = buf;

    (void)region;
    for (size_t i = 0; i < len; i++) {
        out[i] = mclw_command_get_register(&s->image, (uint16_t)(s->held_base + offset + i));
    }
    return 0;
}

static int at45db_write_region(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf,
    size_t len) {
    struct at45db_state *s = inst->state;

    (void)region;
    (void)mclw_command_set_registers(&s->image, (uint16_t)(s->held_base + offset), buf, len);
    publish(s);
    return 0;
}

static const struct mclw_model_ops at45db_ops = {
    .init = at45db_init,
    .deinit = at45db_deinit,
    .reset = at45db_reset,
    .get = at45db_get,
    .set = at45db_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = at45db_read_region,
    .write_region = at45db_write_region,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_at45db041e = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "at45db041e",
    .family = "at45db",
    .config = NULL,
    .properties = at45db_properties,
    .property_count = sizeof(at45db_properties) / sizeof(at45db_properties[0]),
    .regions = at45db_regions,
    .region_count = sizeof(at45db_regions) / sizeof(at45db_regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = at45db_bindings,
    .binding_count = sizeof(at45db_bindings) / sizeof(at45db_bindings[0]),
    .state_size = sizeof(struct at45db_state),
    .ops = &at45db_ops,
};
