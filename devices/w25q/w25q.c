// SPDX-License-Identifier: Apache-2.0 OR MIT
// Winbond W25Q16JV serial NOR flash on SPI, served as a command image: Read
// Data (03h) is the image's memory read, every other instruction arrives as
// a window after chip select rises and is executed here. Section numbers
// are the W25Q16JV datasheet's, Revision D.

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

#include "w25q.h"

// 9.6 AC Electrical Characteristics, typical: tPP 0.4 ms, tSE 45 ms, tBE1
// 120 ms, tBE2 150 ms, tCE 5 s. The model is busy that long on the
// simulation clock, which ticks once a millisecond.
#define T_PP_US 400u
#define T_SE_US 45000u
#define T_BE1_US 120000u
#define T_BE2_US 150000u
#define T_CE_US 5000000u

// 8.1, Manufacturer and Device Identification: 16M-bit, 2 MiB; JEDEC ID
// EFh 40h 15h.
#define CAPACITY_BYTES (2u * 1024u * 1024u)
#define JEDEC_ID 0xef4015u

enum {
    PROP_STATUS,
    PROP_WRITE_ENABLED,
    PROP_BUSY,
    PROP_POWER_DOWN,
    PROP_JEDEC_ID,
    PROP_READS,
    PROP_PROGRAMS,
    PROP_ERASES,
    PROP_LOST,
    PROP_IGNORED,
    PROP_SPI_MODE,
    PROP_ID_READ,
    PROP_HELD_BASE,
};

enum {
    REGION_ARRAY,
};

enum {
    BINDING_SPI,
};

struct w25q_state {
    struct mclw_command_image image;
    struct mclw_spi_target *spi;
    uint8_t status;
    bool power_down;
    bool reset_enabled;
    bool busy;
    uint64_t busy_until_us;
    uint64_t now_us;
    uint32_t reads;
    uint32_t programs;
    uint32_t erases;
    uint32_t lost;
    uint32_t ignored;
    // Whether the image answers Read JEDEC ID, and the address of the first
    // held byte.
    bool id_read;
    uint32_t held_base;
};

static const struct mclw_property_desc w25q_properties[] = {
    { .name = "status", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "write_enabled", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "busy", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "power_down", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "jedec_id", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 0xffffff },
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
    { .name = "id_read", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "held_base", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = CAPACITY_BYTES - W25Q_HELD_BYTES },
};

static const struct mclw_region_desc w25q_regions[] = {
    { .name = "array", .size = W25Q_HELD_BYTES, .access = MCLW_ACCESS_RW },
};

// 6.1: SPI modes 0 and 3, MSB first.
static const struct mclw_binding_desc w25q_bindings[] = {
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
    return ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
}

// The image holds the pages from held_base on, named by address bits
// 15..8 as the engine serves them; a byte outside them has nowhere to go.
static uint8_t *held(struct w25q_state *s, uint32_t address) {
    if (address < s->held_base || address - s->held_base >= W25Q_HELD_BYTES) {
        return NULL;
    }
    return mclw_command_register(&s->image, (uint16_t)address);
}

static void publish(struct w25q_state *s) {
    mclw_command_set_status(&s->image, s->status);
    mclw_command_publish(&s->image);
}

static void start(struct w25q_state *s, uint32_t duration_us) {
    s->status |= W25Q_SR1_BUSY;
    s->busy = true;
    s->busy_until_us = s->now_us + duration_us;
}

// 8.2, Page Program (02h): the data goes into the addressed page from the given byte on,
// wrapping to the start of that page, a later byte for the same address
// replacing an earlier one; programming only clears bits.
static void page_program(struct w25q_state *s, const uint8_t *bytes, size_t len) {
    uint32_t address = address_of(bytes);
    uint32_t page = address & ~(W25Q_PAGE_SIZE - 1u);
    uint8_t latch[W25Q_PAGE_SIZE];

    memset(latch, 0xff, sizeof(latch));
    for (size_t i = 3; i < len; i++) {
        latch[(address + (uint32_t)(i - 3u)) & (W25Q_PAGE_SIZE - 1u)] = bytes[i];
    }
    for (uint32_t i = 0; i < W25Q_PAGE_SIZE; i++) {
        uint8_t *cell = held(s, page | i);

        if (cell != NULL) {
            *cell &= latch[i];
        } else if (latch[i] != 0xffu) {
            s->lost++;
        }
    }
    s->programs++;
    start(s, T_PP_US);
}

// 8.3, Sector Erase (20h), and the block and chip erases after it: every
// byte of the aligned block reads FFh afterwards.
static void erase(struct w25q_state *s, uint32_t address, uint32_t size, uint32_t duration_us) {
    uint32_t first = address & ~(size - 1u);

    for (uint32_t at = 0; at < W25Q_HELD_BYTES; at++) {
        if (s->held_base + at >= first && s->held_base + at < first + size) {
            *held(s, s->held_base + at) = 0xff;
        }
    }
    s->erases++;
    start(s, duration_us);
}

// 8.2, Write Enable (06h): an erase or program needs WEL; once accepted, WEL
// clears when it completes. While BUSY, every instruction but Read Status is
// ignored (7.1, BUSY); after Power-down only Release Power-down is taken
// (8.3, Power-down (B9h)).
static void w25q_on_window(struct mclw_command_image *image, const uint8_t *bytes, size_t len, bool busy,
    void *ctx) {
    struct w25q_state *s = ctx;
    uint8_t opcode = len != 0 ? bytes[0] : 0;
    bool reset_enabled = s->reset_enabled;

    (void)image;
    (void)busy;
    s->reset_enabled = false;
    if (len == 0) {
        return;
    }
    if (s->busy && opcode != W25Q_READ_STATUS_1) {
        s->ignored++;
    } else if (s->power_down && opcode != W25Q_RELEASE_POWER_DOWN) {
        s->ignored++;
    } else {
        bool wel = (s->status & W25Q_SR1_WEL) != 0;

        switch (opcode) {
            case W25Q_WRITE_ENABLE:
                s->status |= W25Q_SR1_WEL;
                break;
            case W25Q_WRITE_DISABLE:
                s->status &= (uint8_t)~W25Q_SR1_WEL;
                break;
            case W25Q_READ_STATUS_1:
                break;
            case W25Q_READ_DATA:
                s->reads++;
                break;
            case W25Q_READ_JEDEC_ID:
                if (!s->id_read) {
                    s->ignored++;
                }
                break;
            case W25Q_PAGE_PROGRAM:
                if (wel && len >= 5) {
                    page_program(s, &bytes[1], len - 1);
                } else {
                    s->ignored++;
                }
                break;
            case W25Q_SECTOR_ERASE:
            case W25Q_BLOCK_ERASE_32K:
            case W25Q_BLOCK_ERASE_64K:
                if (wel && len == 4) {
                    uint32_t address = address_of(&bytes[1]);
                    if (opcode == W25Q_SECTOR_ERASE) {
                        erase(s, address, W25Q_SECTOR_SIZE, T_SE_US);
                    } else if (opcode == W25Q_BLOCK_ERASE_32K) {
                        erase(s, address, 32u * 1024u, T_BE1_US);
                    } else {
                        erase(s, address, 64u * 1024u, T_BE2_US);
                    }
                } else {
                    s->ignored++;
                }
                break;
            case W25Q_CHIP_ERASE:
            case W25Q_CHIP_ERASE_ALT:
                if (wel && len == 1) {
                    erase(s, 0, CAPACITY_BYTES, T_CE_US);
                } else {
                    s->ignored++;
                }
                break;
            case W25Q_POWER_DOWN:
                s->power_down = true;
                break;
            case W25Q_RELEASE_POWER_DOWN:
                s->power_down = false;
                break;
            case W25Q_ENABLE_RESET:
                s->reset_enabled = true;
                break;
            // 8.3, Enable Reset (66h) and Reset Device (99h): the pair ends any
            // operation and clears the volatile status bits.
            case W25Q_RESET:
                if (reset_enabled) {
                    s->status = 0;
                    s->busy = false;
                } else {
                    s->ignored++;
                }
                break;
            default:
                s->ignored++;
                break;
        }
    }
    mclw_trace(mclw_instance_from_state(s), MCLW_TRACE_COMMAND, opcode, (uint16_t)len, 0);
    // Every window publishes, so what the engine serves is the part's own
    // status once it stops holding BUSY for this window.
    publish(s);
}

static void build_image(struct w25q_state *s) {
    static const uint8_t busy_opcodes[] = {
        W25Q_PAGE_PROGRAM, W25Q_SECTOR_ERASE, W25Q_BLOCK_ERASE_32K, W25Q_BLOCK_ERASE_64K,
        W25Q_CHIP_ERASE, W25Q_CHIP_ERASE_ALT,
    };

    mclw_command_init(&s->image, W25Q_READ_DATA, W25Q_READ_DATA);
    mclw_command_set_memory_read(&s->image, W25Q_READ_DATA);
    mclw_command_set_filler(&s->image, 0xff);
    mclw_command_set_busy_status(&s->image, W25Q_SR1_BUSY);
    for (size_t i = 0; i < sizeof(busy_opcodes); i++) {
        mclw_command_add_busy_opcode(&s->image, busy_opcodes[i]);
    }
    for (uint8_t page = 0; page < W25Q_HELD_PAGES; page++) {
        uint8_t number = (uint8_t)((s->held_base >> 8) + page);

        (void)mclw_command_add_page(&s->image, number);
        memset(mclw_command_register(&s->image, (uint16_t)(number << 8)), 0xff, W25Q_PAGE_SIZE);
    }
    // 8.2, Read JEDEC ID (9Fh): the manufacturer and the two device ID bytes
    // follow the instruction at once.
    if (s->id_read) {
        static const uint8_t id[] = { JEDEC_ID >> 16, (JEDEC_ID >> 8) & 0xffu, JEDEC_ID & 0xffu };

        mclw_command_set_immediate_response(&s->image);
        (void)mclw_command_add_response(&s->image, W25Q_READ_JEDEC_ID);
        (void)mclw_command_set_response(&s->image, W25Q_READ_JEDEC_ID, id, sizeof(id));
    }
    mclw_command_set_on_window(&s->image, w25q_on_window, s);
}

// The image again, for a new id_read or held_base: the held pages read
// erased, as at attach.
static int rebuild(struct w25q_state *s) {
    mclw_hal_spi_target_disarm(s->spi);
    build_image(s);
    publish(s);
    return mclw_hal_spi_target_arm_command(s->spi, &s->image, NULL);
}

static void power_on(struct w25q_state *s) {
    s->status = 0;
    s->busy = false;
    s->power_down = false;
    s->reset_enabled = false;
}

static int w25q_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct w25q_state *s = inst->state;

    s->spi = bindings[BINDING_SPI].spi;
    s->now_us = mclw_now_us();
    s->id_read = false;
    s->held_base = 0;
    power_on(s);
    build_image(s);
    return mclw_hal_spi_target_arm_command(s->spi, &s->image, NULL);
}

static void w25q_deinit(struct mclw_instance *inst) {
    struct w25q_state *s = inst->state;

    mclw_hal_spi_target_disarm(s->spi);
    s->spi = NULL;
}

// A power cycle keeps the array, which is non-volatile; factory erases it.
static int w25q_reset(struct mclw_instance *inst, bool factory) {
    struct w25q_state *s = inst->state;

    power_on(s);
    if (factory) {
        for (uint32_t at = 0; at < W25Q_HELD_BYTES; at++) {
            *held(s, s->held_base + at) = 0xff;
        }
    }
    s->reads = 0;
    s->programs = 0;
    s->erases = 0;
    s->lost = 0;
    s->ignored = 0;
    publish(s);
    return 0;
}

static int w25q_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct w25q_state *s = inst->state;

    switch (property) {
        case PROP_STATUS:
            out->v[0] = s->status;
            return 0;
        case PROP_WRITE_ENABLED:
            out->v[0] = (s->status & W25Q_SR1_WEL) != 0;
            return 0;
        case PROP_BUSY:
            out->v[0] = s->busy;
            return 0;
        case PROP_POWER_DOWN:
            out->v[0] = s->power_down;
            return 0;
        case PROP_JEDEC_ID:
            out->v[0] = JEDEC_ID;
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
        case PROP_ID_READ:
            out->v[0] = s->id_read;
            return 0;
        case PROP_HELD_BASE:
            out->v[0] = s->held_base;
            return 0;
        default:
            return -EINVAL;
    }
}

// held_base is a page boundary whose held pages stay within one 64 KiB
// block, since the engine names a page by address bits 15..8.
static int w25q_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct w25q_state *s = inst->state;

    switch (property) {
        case PROP_SPI_MODE:
            return mclw_spi_mode_set(inst, BINDING_SPI, in);
        case PROP_ID_READ:
            s->id_read = in->v[0] != 0;
            return rebuild(s);
        case PROP_HELD_BASE: {
            uint32_t base = (uint32_t)in->v[0];

            if ((base & (W25Q_PAGE_SIZE - 1u)) != 0 || (base & 0xffffu) + W25Q_HELD_BYTES > 0x10000u ||
                base > CAPACITY_BYTES - W25Q_HELD_BYTES) {
                return -EINVAL;
            }
            s->held_base = base;
            return rebuild(s);
        }
        default:
            return -EINVAL;
    }
}

static int w25q_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    struct w25q_state *s = inst->state;
    uint8_t *out = buf;

    (void)region;
    for (size_t i = 0; i < len; i++) {
        out[i] = mclw_command_get_register(&s->image, (uint16_t)(s->held_base + offset + i));
    }
    return 0;
}

// The host writes the array as a programmer would have left it.
static int w25q_write_region(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf,
    size_t len) {
    struct w25q_state *s = inst->state;

    (void)region;
    (void)mclw_command_set_registers(&s->image, (uint16_t)(s->held_base + offset), buf, len);
    publish(s);
    return 0;
}

static void w25q_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct w25q_state *s = inst->state;

    s->now_us = now_us;
    if (s->busy && now_us >= s->busy_until_us) {
        s->busy = false;
        s->status &= (uint8_t)~(W25Q_SR1_BUSY | W25Q_SR1_WEL);
        publish(s);
    }
}

static const struct mclw_model_ops w25q_ops = {
    .init = w25q_init,
    .deinit = w25q_deinit,
    .reset = w25q_reset,
    .get = w25q_get,
    .set = w25q_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = w25q_read_region,
    .write_region = w25q_write_region,
    .time_advanced = w25q_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_w25q16jv = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "w25q16jv",
    .family = "w25q",
    .config = NULL,
    .properties = w25q_properties,
    .property_count = sizeof(w25q_properties) / sizeof(w25q_properties[0]),
    .regions = w25q_regions,
    .region_count = sizeof(w25q_regions) / sizeof(w25q_regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = w25q_bindings,
    .binding_count = sizeof(w25q_bindings) / sizeof(w25q_bindings[0]),
    .state_size = sizeof(struct w25q_state),
    .ops = &w25q_ops,
};
