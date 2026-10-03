// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/regs/dreq.h"
#include "pico/time.h"

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_target.h"

#define SPI_PIO pio1
#define SPI_TARGET_COUNT 2u
#define SPI_PROGRAM_LENGTH 32u

#define SPI_STREAM_ENTRY 0u
#define SPI_STREAM_ACTIVE_ENTRY 4u
#define SPI_STREAM_WRAP_TARGET 6u
#define SPI_STREAM_WRAP_TOP 11u
#define SPI_REGMAP_WRAP_TARGET 12u
#define SPI_REGMAP_WRAP_TOP 10u
#define SPI_REGMAP_ENTRY 14u
#define SPI_MONITOR_ENTRY 28u
#define SPI_MONITOR_WRAP_TOP 31u
#define SPI_REST_LEAD 6u
#define SPI_REST_SAMPLE 7u
#define SPI_REST_SHIFT 9u
#define SPI_DUMMY_SLOT 25u
#define SPI_REST_FIRST 26u

#define SPI_BUFFER_BYTES 264u
#define SPI_TABLE_SLOTS 8u
#define SPI_TABLE_ALIGN 2048u
#define SPI_TABLE_SHIFT 11u
#define SPI_BANK_BYTES ((uint32_t)MCLW_REGMAP_BANK_SIZE)
#define SPI_FRAME_BYTES ((uint32_t)MCLW_STREAM_FRAME_MAX)

#define SPI_REGMAP_CLOCK_HZ 10714285u
#define SPI_STREAM_CLOCK_HZ 15000000u

// A stream's frame is re-armed in idle only when no window has ended this
// recently: re-arming stops the state machine, so a host that is clocking
// windows back to back must not be re-armed under; its window ends re-arm
// the frame instead.
#define SPI_STREAM_BURST_US 2000u
#define SPI_RELEASE_BUDGET_US 16u
#define SPI_RELEASE_STABLE_US 1u
#define SPI_RELEASE_SETTLE_US 10u
#define SPI_RELEASE_POLLS 2
#define SPI_LOADER_WAIT_LOOPS 64u

#define SPI_DMA_ERROR_BITS (DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS | \
    DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS)

enum spi_image {
    SPI_IMAGE_NONE,
    SPI_IMAGE_REGMAP,
    SPI_IMAGE_STREAM,
    SPI_IMAGE_COMMAND,
};

struct spi_descriptor {
    uint32_t count;
    uint32_t source;
};

// A command image is served by a DMA program, not by the CPU. The stream
// state machine shifts every MOSI byte into its RX FIFO and every MISO byte
// out of its TX FIFO. The loader channel L copies 16-byte control blocks,
// one at a time, into the worker channel W, which chains back to L when the
// block is done. The blocks read the opcode, offset or address bytes from
// the RX FIFO and patch them into the low bytes of later blocks and of
// address templates (SRAM honours byte writes, DMA registers do not), which
// is how a table lookup and an address composition happen without the CPU.
// The data channel D streams the answer into the TX FIFO.
//
//   prologue  2 status bytes out; opcode -> &optab[op] -> jump -> L
//   optab[op] picks a list: write, response, buffer read, register read
//   pagetab   maps address bits 15..8 to the page's address bits 15..8
//
// A chain trigger fires before the last write of the finished block has
// landed (measured on the bench). So the jump to a list writes L's
// READ_ADDR through its trigger alias instead of chaining, and a block
// that W patches is never the very next one L loads: a spacer block sits
// between them, or the patch is written SPI_COMMAND_STEP_REPEAT times so
// the first write has landed. Without these, about one window in a
// thousand loaded a block from a stale address.
//
// A response read runs six blocks between the opcode's last bit and its
// first data byte reaching the TX FIFO, which must happen within the one
// byte time the status byte gives: about 600 ns, measured as a clean pass
// at 12.5 MHz and a failure at 15 MHz. That sets the qualified clock.
//
// The structure is one 2 KiB table slot; the register pages are two more,
// adjacent (see command_layout).
struct spi_cb {
    uint32_t read;
    uint32_t write;
    uint32_t count;
    uint32_t ctrl;
};

// DMA AL1 order: CTRL, READ_ADDR, WRITE_ADDR, TRANS_COUNT_TRIG.
struct spi_dblock {
    uint32_t ctrl;
    uint32_t read;
    uint32_t write;
    uint32_t count;
};

enum {
    SPI_LIST_WRITE = 0,
    SPI_LIST_RESPONSE = 3,
    SPI_LIST_BUFFER = 8,
    SPI_LIST_REGISTER = 13,
    SPI_LIST_CBS = 21,
};

enum {
    SPI_PROLOGUE_STATUS,
    SPI_PROLOGUE_OPCODE,
    SPI_PROLOGUE_SPACER,
    SPI_PROLOGUE_LOOKUP,
    SPI_PROLOGUE_JUMP,
    SPI_PROLOGUE_CBS,
};

enum {
    SPI_DBLOCK_WRITE,
    SPI_DBLOCK_RESPONSE,
    SPI_DBLOCK_BUFFER,
    SPI_DBLOCK_REGISTER,
    SPI_DBLOCKS,
};

#define SPI_COMMAND_PAGE_SLOTS (2 * MCLW_COMMAND_PAGES_MAX + 2)
#define SPI_COMMAND_NO_OPCODE 0xffu
#define SPI_COMMAND_STATUS_RUN 300u
#define SPI_COMMAND_DATA_RUN 256u
#define SPI_COMMAND_CLOCK_HZ 8000000u
#define SPI_COMMAND_STEP_REPEAT 4u

struct spi_command_block {
    uint8_t optab[256];
    uint8_t resptab[256];
    uint8_t pagetab[256];
    // List heads must sit within 256 bytes of `lists`: the jump template
    // takes the head's low byte from optab.
    struct spi_cb lists[SPI_LIST_CBS];
    struct spi_cb prologue[SPI_PROLOGUE_CBS];
    struct spi_dblock dblock[SPI_DBLOCKS];
    uint32_t status;
    uint32_t jump;
    uint32_t scratch;
    uint8_t unused[0x500 - 0x4ec];
    uint8_t responses[MCLW_COMMAND_RESPONSES_MAX][MCLW_COMMAND_RESPONSE_MAX];
    uint8_t unused2[0x600 - 0x580];
    // Two copies, so a read from any offset runs on without wrapping.
    uint8_t buffer[2 * MCLW_COMMAND_BUFFER_SIZE];
};

_Static_assert(sizeof(struct spi_command_block) == 2048u, "command block is one table slot");
_Static_assert(offsetof(struct spi_command_block, lists) % 256u == 0u, "list base is page aligned");
_Static_assert(offsetof(struct spi_command_block, buffer) % 256u == 0u, "buffer is page aligned");
_Static_assert(SPI_LIST_REGISTER * sizeof(struct spi_cb) < 255u, "list heads fit a byte");

struct spi_bus {
    uint8_t sck;
    uint8_t mosi;
    uint8_t miso;
    uint8_t open_count;

    uint16_t program_words[SPI_PROGRAM_LENGTH];
    pio_program_t program;
    bool program_loaded;
    uint8_t program_offset;
    uint8_t program_sck;
    uint8_t program_class;
    uint16_t program_dummy;

    bool release_installed;
    bool dma_installed;
    bool poll_registered;

    uint8_t table_owner[SPI_TABLE_SLOTS];
};

struct mclw_spi_target {
    struct spi_bus *bus;
    const void *owner;
    uint8_t cs;
    uint8_t mode;
    bool msb_first;
    uint8_t sm;
    uint8_t monitor_sm;
    int8_t tx_dma;
    int8_t rx_dma;
    int8_t loader_dma;

    enum spi_image image;
    struct mclw_regmap *map;
    struct mclw_regmap_framing framing;
    struct mclw_stream *stream;
    struct mclw_command_image *command;
    int8_t busy_pin;
    volatile bool busy;
    bool window_busy;
    volatile bool shadow_pending;
    volatile bool record_full;
    uint8_t page_slot[MCLW_COMMAND_PAGES_MAX];
    uint16_t record_len;
    bool record_busy;
    struct mclw_work record_work;

    uint8_t banks;
    uint8_t table[MCLW_REGMAP_BANKS_MAX];
    uint8_t bank;

    volatile bool window_open;
    volatile bool command_taken;
    volatile bool wait_idle;
    volatile bool frame_pending;
    bool released;
    uint32_t last_release_us;
    uint8_t release_polls;
    uint32_t payload_ctrl;
    uint32_t tx_ctrl;

    uint8_t tx_buf[SPI_BUFFER_BYTES];
    uint8_t capture[SPI_BUFFER_BYTES];
    uint8_t record[MCLW_COMMAND_WINDOW_MAX + 1u];
    uint8_t delivered[MCLW_COMMAND_WINDOW_MAX + 1u];

    struct mclw_spi_target_stats stats;
};

// pio_sm_set_enabled() is not inlined and so would run from flash.
static inline __attribute__((always_inline)) void sm_enable(uint sm, bool enabled) {
    if (enabled) {
        hw_set_bits(&SPI_PIO->ctrl, 1u << sm);
    } else {
        hw_clear_bits(&SPI_PIO->ctrl, 1u << sm);
    }
}

static struct spi_bus spi_bus;
static struct mclw_spi_target targets[SPI_TARGET_COUNT];

union spi_table {
    struct spi_descriptor descriptors[MCLW_REGMAP_BANK_SIZE];
    struct spi_command_block command;
};

_Static_assert(SPI_COMMAND_PAGE_SLOTS * MCLW_COMMAND_PAGE_SIZE == 2 * sizeof(union spi_table),
    "the page slots fill two table slots");

static union spi_table table_pool[SPI_TABLE_SLOTS] __attribute__((aligned(SPI_TABLE_ALIGN)));

static const uint8_t wait_slots[] = { 6u, 7u, 18u, 19u };

static uint8_t __not_in_flash_func(reverse_byte)(uint8_t value) {
    uint32_t v = value;

    v = ((v & 0x55u) << 1) | ((v >> 1) & 0x55u);
    v = ((v & 0x33u) << 2) | ((v >> 2) & 0x33u);
    v = ((v & 0x0fu) << 4) | ((v >> 4) & 0x0fu);
    return (uint8_t)v;
}

static uint8_t mode_class(uint8_t mode) {
    return (mode == 1u || mode == 2u) ? 1u : 0u;
}

static uint8_t __not_in_flash_func(wire_byte)(const struct mclw_spi_target *t, uint8_t value) {
    return t->msb_first ? reverse_byte(value) : value;
}

static void encode_program(struct spi_bus *bus, uint8_t sck, uint8_t cls) {
    uint16_t *p = bus->program_words;
    bool lead = cls != 0u;

    p[0] = (uint16_t)pio_encode_out(pio_pins, 1u);
    p[1] = (uint16_t)pio_encode_jmp_pin(3u);
    p[2] = (uint16_t)pio_encode_jmp(1u);
    p[3] = (uint16_t)pio_encode_jmp_pin(3u);
    p[4] = (uint16_t)pio_encode_set(pio_pindirs, 1u);
    p[5] = (uint16_t)pio_encode_set(pio_x, 7u);
    p[6] = (uint16_t)pio_encode_wait_gpio(lead, sck);
    p[7] = (uint16_t)pio_encode_wait_gpio(!lead, sck);
    p[8] = (uint16_t)pio_encode_in(pio_pins, 1u);
    p[9] = (uint16_t)pio_encode_out(pio_pins, 1u);
    p[10] = (uint16_t)pio_encode_jmp_x_dec(6u);
    p[11] = (uint16_t)pio_encode_set(pio_x, 7u);
    p[12] = (uint16_t)pio_encode_push(false, false);
    p[13] = (uint16_t)pio_encode_jmp(5u);
    p[14] = (uint16_t)pio_encode_pull(false, false);
    p[15] = (uint16_t)pio_encode_jmp_pin(15u);
    p[16] = (uint16_t)pio_encode_in(pio_y, 21u);
    p[17] = (uint16_t)pio_encode_set(pio_pindirs, 1u);
    p[18] = (uint16_t)pio_encode_wait_gpio(lead, sck);
    p[19] = (uint16_t)pio_encode_wait_gpio(!lead, sck);
    p[20] = (uint16_t)pio_encode_in(pio_pins, 1u);
    p[21] = (uint16_t)pio_encode_out(pio_pins, 1u);
    p[22] = (uint16_t)pio_encode_jmp_not_osre(18u);
    p[23] = (uint16_t)pio_encode_in(pio_null, 3u);
    p[24] = (uint16_t)pio_encode_push(false, true);
    p[25] = bus->program_dummy;
    p[26] = (uint16_t)pio_encode_out(pio_pins, 1u);
    p[27] = (uint16_t)pio_encode_jmp(5u);
    // The monitor raises BUSY for a command image on its own SET pin; a
    // monitor with no SET pin configured skips it.
    p[28] = (uint16_t)pio_encode_wait_pin(false, 0u);
    p[29] = (uint16_t)pio_encode_wait_pin(true, 0u);
    p[30] = (uint16_t)pio_encode_set(pio_pins, 1u);
    p[31] = (uint16_t)pio_encode_irq_set(true, 0u);

    bus->program.instructions = bus->program_words;
    bus->program.length = (uint8_t)SPI_PROGRAM_LENGTH;
    bus->program.origin = -1;
    bus->program.pio_version = 0;
}

static uint16_t dummy_instruction(const struct mclw_regmap_framing *framing, uint8_t filler) {
    if (framing->read_dummy_bytes == 0u) {
        return (uint16_t)pio_encode_nop();
    }
    return filler == 0u ? (uint16_t)pio_encode_mov(pio_osr, pio_null)
        : (uint16_t)pio_encode_mov_not(pio_osr, pio_null);
}

static void program_patch_dummy(struct spi_bus *bus, uint16_t instruction) {
    bus->program_dummy = instruction;
    bus->program_words[SPI_DUMMY_SLOT] = instruction;
    SPI_PIO->instr_mem[bus->program_offset + SPI_DUMMY_SLOT] = instruction;
}

static void program_patch(struct spi_bus *bus, uint8_t sck, uint8_t cls) {
    encode_program(bus, sck, cls);
    for (size_t i = 0; i < sizeof(wait_slots); i++) {
        uint8_t slot = wait_slots[i];

        SPI_PIO->instr_mem[bus->program_offset + slot] = bus->program_words[slot];
    }
    bus->program_sck = sck;
    bus->program_class = cls;
}

static int bus_load_program(struct spi_bus *bus, uint8_t sck, uint8_t cls) {
    if (bus->program_loaded) {
        return 0;
    }
    bus->program_dummy = (uint16_t)pio_encode_nop();
    encode_program(bus, sck, cls);
    if (!pio_can_add_program(SPI_PIO, &bus->program)) {
        return -ENOSPC;
    }
    bus->program_offset = (uint8_t)pio_add_program(SPI_PIO, &bus->program);
    bus->program_sck = sck;
    bus->program_class = cls;
    bus->program_loaded = true;
    return 0;
}

static void bus_unload_program(struct spi_bus *bus) {
    if (!bus->program_loaded) {
        return;
    }
    pio_remove_program(SPI_PIO, &bus->program, bus->program_offset);
    bus->program_loaded = false;
}

static bool __not_in_flash_func(bus_miso_held)(const struct mclw_spi_target *t) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        const struct mclw_spi_target *o = &targets[i];

        if (o != t && o->bus == t->bus && o->window_open) {
            return true;
        }
    }
    return false;
}

static void __not_in_flash_func(miso_release)(const struct mclw_spi_target *t) {
    if (bus_miso_held(t)) {
        return;
    }
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_set(pio_pindirs, 0u));
}

static void __not_in_flash_func(stop_channel)(int8_t channel) {
    dma_channel_hw_t *hw;

    if (channel < 0) {
        return;
    }
    hw = dma_channel_hw_addr((uint)channel);
    hw_write_masked(&hw->al1_ctrl, ((uint32_t)channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB),
        DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS | DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort((uint)channel);
    hw_set_bits(&hw->al1_ctrl, DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS);
    dma_hw->intr = 1u << channel;
}

static void __not_in_flash_func(stop_dma)(struct mclw_spi_target *t) {
    if (t->rx_dma >= 0) {
        dma_channel_set_irq1_enabled((uint)t->rx_dma, false);
    }
    stop_channel(t->rx_dma);
    stop_channel(t->tx_dma);
    stop_channel(t->loader_dma);
    for (uint32_t i = 0; i < 16u; i++) {
        pio_sm_clear_fifos(SPI_PIO, t->sm);
        busy_wait_at_least_cycles(64);
        if (pio_sm_get_tx_fifo_level(SPI_PIO, t->sm) == 0u && pio_sm_get_rx_fifo_level(SPI_PIO, t->sm) == 0u) {
            return;
        }
    }
}

static bool __not_in_flash_func(dma_faulted)(const struct mclw_spi_target *t) {
    uint32_t bits = 0;

    if (t->tx_dma >= 0) {
        bits |= dma_channel_hw_addr((uint)t->tx_dma)->ctrl_trig;
    }
    if (t->rx_dma >= 0) {
        bits |= dma_channel_hw_addr((uint)t->rx_dma)->ctrl_trig;
    }
    if (t->loader_dma >= 0) {
        bits |= dma_channel_hw_addr((uint)t->loader_dma)->ctrl_trig;
    }
    return (bits & SPI_DMA_ERROR_BITS) != 0u;
}

static void __not_in_flash_func(quiesce)(const struct mclw_spi_target *t) {
    uint32_t previous;
    uint32_t stable = 0;

    if (t->rx_dma < 0) {
        return;
    }
    previous = dma_channel_hw_addr((uint)t->rx_dma)->transfer_count;
    busy_wait_at_least_cycles(32);
    {
        uint8_t pc = pio_sm_get_pc(SPI_PIO, t->sm);
        bool rest = pc == (uint8_t)(t->bus->program_offset + SPI_REST_LEAD) ||
            pc == (uint8_t)(t->bus->program_offset + SPI_REST_SAMPLE) ||
            pc == (uint8_t)(t->bus->program_offset + SPI_REST_SHIFT) ||
            pc == (uint8_t)(t->bus->program_offset + SPI_REST_FIRST);

        if (rest && pio_sm_is_rx_fifo_empty(SPI_PIO, t->sm) &&
            dma_channel_hw_addr((uint)t->rx_dma)->transfer_count == previous) {
            return;
        }
    }
    for (uint32_t i = 0; i < SPI_RELEASE_BUDGET_US; i++) {
        uint32_t remaining;
        uint8_t pc;
        bool rest;

        // busy_wait_us_32() would run from flash; the timer read is inline.
        for (uint32_t start = time_us_32(); time_us_32() - start < 1u;) {
        }
        remaining = dma_channel_hw_addr((uint)t->rx_dma)->transfer_count;
        pc = pio_sm_get_pc(SPI_PIO, t->sm);
        rest = pc == (uint8_t)(t->bus->program_offset + SPI_REST_LEAD) ||
            pc == (uint8_t)(t->bus->program_offset + SPI_REST_SAMPLE) ||
            pc == (uint8_t)(t->bus->program_offset + SPI_REST_SHIFT) ||
            pc == (uint8_t)(t->bus->program_offset + SPI_REST_FIRST);
        if (rest && remaining == previous && pio_sm_is_rx_fifo_empty(SPI_PIO, t->sm)) {
            stable++;
            if (stable >= SPI_RELEASE_STABLE_US) {
                return;
            }
        } else {
            stable = 0;
        }
        previous = remaining;
    }
}

static void configure_sm(struct mclw_spi_target *t, enum spi_image image) {
    const struct spi_bus *bus = t->bus;
    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_out_pins(&c, bus->miso, 1u);
    sm_config_set_set_pins(&c, bus->miso, 1u);
    sm_config_set_in_pins(&c, bus->mosi);
    sm_config_set_jmp_pin(&c, t->cs);
    if (image == SPI_IMAGE_REGMAP) {
        sm_config_set_out_shift(&c, false, false, 8u);
        sm_config_set_in_shift(&c, false, false, 32u);
        sm_config_set_wrap(&c, bus->program_offset + SPI_REGMAP_WRAP_TARGET,
            bus->program_offset + SPI_REGMAP_WRAP_TOP);
        pio_sm_init(SPI_PIO, t->sm, bus->program_offset + SPI_REGMAP_ENTRY, &c);
    } else {
        // A command image is MSB first in the image; a stream frame is
        // bit-reversed into tx_buf instead.
        sm_config_set_out_shift(&c, image != SPI_IMAGE_COMMAND, false, 8u);
        sm_config_set_in_shift(&c, false, true, 8u);
        sm_config_set_wrap(&c, bus->program_offset + SPI_STREAM_WRAP_TARGET,
            bus->program_offset + SPI_STREAM_WRAP_TOP);
        pio_sm_init(SPI_PIO, t->sm, bus->program_offset + SPI_STREAM_ENTRY, &c);
    }
    pio_sm_set_consecutive_pindirs(SPI_PIO, t->sm, bus->miso, 1u, false);
}

static void __not_in_flash_func(set_autoshift)(const struct mclw_spi_target *t, bool pull, bool push,
    uint32_t threshold, uint32_t pull_threshold) {
    const uint32_t mask = PIO_SM0_SHIFTCTRL_AUTOPUSH_BITS | PIO_SM0_SHIFTCTRL_AUTOPULL_BITS |
        PIO_SM0_SHIFTCTRL_PUSH_THRESH_BITS | PIO_SM0_SHIFTCTRL_PULL_THRESH_BITS;
    uint32_t field = threshold & 0x1fu;
    uint32_t value = (field << PIO_SM0_SHIFTCTRL_PUSH_THRESH_LSB) |
        ((pull_threshold & 0x1fu) << PIO_SM0_SHIFTCTRL_PULL_THRESH_LSB);

    if (pull) {
        value |= PIO_SM0_SHIFTCTRL_AUTOPULL_BITS;
    }
    if (push) {
        value |= PIO_SM0_SHIFTCTRL_AUTOPUSH_BITS;
    }
    SPI_PIO->sm[t->sm].shiftctrl = (SPI_PIO->sm[t->sm].shiftctrl & ~mask) | value;
}

static volatile void *__not_in_flash_func(tx_fifo_msb)(const struct mclw_spi_target *t) {
    return (volatile uint8_t *)&SPI_PIO->txf[t->sm] + 3;
}

static void __not_in_flash_func(arm_capture)(struct mclw_spi_target *t, bool trigger) {
    dma_channel_config c = dma_channel_get_default_config((uint)t->rx_dma);

    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_high_priority(&c, true);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(SPI_PIO, t->sm, false));
    t->payload_ctrl = channel_config_get_ctrl_value(&c);
    if (trigger) {
        dma_channel_configure((uint)t->rx_dma, &c, t->capture, &SPI_PIO->rxf[t->sm],
            SPI_BUFFER_BYTES, true);
    }
}

static void __not_in_flash_func(regmap_prepare)(struct mclw_spi_target *t, bool join) {
    struct mclw_regmap *map = t->map;
    const struct spi_descriptor *table;
    dma_channel_config c;
    uint8_t bank = map->active_bank;
    bool idle_wait;
    bool active;

    if (bank >= t->banks) {
        bank = 0;
    }
    t->bank = bank;
    table = table_pool[t->table[bank]].descriptors;

    sm_enable(t->sm, false);
    stop_dma(t);
    set_autoshift(t, false, false, 32u, 8u);
    pio_sm_clear_fifos(SPI_PIO, t->sm);
    pio_sm_restart(SPI_PIO, t->sm);
    miso_release(t);


    dma_channel_set_irq1_enabled((uint)t->rx_dma, false);
    dma_irqn_acknowledge_channel(1u, (uint)t->rx_dma);

    c = dma_channel_get_default_config((uint)t->tx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_high_priority(&c, true);
    channel_config_set_read_increment(&c, t->framing.auto_increment);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(SPI_PIO, t->sm, true));
    dma_channel_configure((uint)t->tx_dma, &c, tx_fifo_msb(t), t->tx_buf, SPI_BUFFER_BYTES, false);

    c = dma_channel_get_default_config((uint)t->loader_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_high_priority(&c, true);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    dma_channel_configure((uint)t->loader_dma, &c,
        &dma_channel_hw_addr((uint)t->tx_dma)->al3_transfer_count, table, 2u, false);

    c = dma_channel_get_default_config((uint)t->rx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_high_priority(&c, true);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(SPI_PIO, t->sm, false));
    dma_channel_configure((uint)t->rx_dma, &c,
        &dma_channel_hw_addr((uint)t->loader_dma)->al3_read_addr_trig, &SPI_PIO->rxf[t->sm],
        1u, true);
    arm_capture(t, false);
    dma_channel_set_irq1_enabled((uint)t->rx_dma, true);

    pio_sm_put(SPI_PIO, t->sm, (uint32_t)((uintptr_t)table >> SPI_TABLE_SHIFT));
    pio_sm_exec_wait_blocking(SPI_PIO, t->sm, pio_encode_pull(false, true));
    pio_sm_exec_wait_blocking(SPI_PIO, t->sm, pio_encode_out(pio_y, 32u));
    pio_sm_exec_wait_blocking(SPI_PIO, t->sm, pio_encode_mov(pio_isr, pio_null));


    {
        bool low = !gpio_get(t->cs);

        idle_wait = !join && low;
        active = join && low;
    }
    set_autoshift(t, true, false, 32u, 8u);
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_jmp(t->bus->program_offset + SPI_REGMAP_ENTRY));
    t->wait_idle = idle_wait;
    t->window_open = active;
    t->command_taken = false;
    t->release_polls = 0;
    sm_enable(t->sm, !idle_wait);
}

static void __not_in_flash_func(stream_prepare)(struct mclw_spi_target *t, bool join) {
    struct mclw_stream *s = t->stream;
    const uint8_t *frame = s->frame[s->active];
    uint16_t length = s->length > MCLW_STREAM_FRAME_MAX ? MCLW_STREAM_FRAME_MAX : s->length;
    dma_channel_hw_t *tx = dma_channel_hw_addr((uint)t->tx_dma);
    dma_channel_hw_t *rx = dma_channel_hw_addr((uint)t->rx_dma);

    if (s->direction == MCLW_STREAM_OUT) {
        for (uint16_t i = 0; i < length; i++) {
            t->tx_buf[i] = wire_byte(t, frame[i]);
        }
    }

    sm_enable(t->sm, false);
    stop_dma(t);
    set_autoshift(t, false, true, 8u, 8u);
    pio_sm_clear_fifos(SPI_PIO, t->sm);
    pio_sm_restart(SPI_PIO, t->sm);
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_mov(pio_isr, pio_null));
    miso_release(t);

    rx->read_addr = (uintptr_t)&SPI_PIO->rxf[t->sm];
    rx->write_addr = (uintptr_t)t->capture;
    rx->transfer_count = SPI_BUFFER_BYTES;
    rx->ctrl_trig = t->payload_ctrl;
    tx->read_addr = (uintptr_t)t->tx_buf;
    tx->write_addr = (uintptr_t)&SPI_PIO->txf[t->sm];
    tx->transfer_count = SPI_BUFFER_BYTES;
    tx->ctrl_trig = t->tx_ctrl;

    set_autoshift(t, true, true, 8u, 8u);
    {
        bool active = join && !gpio_get(t->cs);

        if (active) {
            pio_sm_exec_wait_blocking(SPI_PIO, t->sm, pio_encode_out(pio_pins, 1u));
        }
        pio_sm_exec(SPI_PIO, t->sm,
            pio_encode_jmp(t->bus->program_offset +
                (active ? SPI_STREAM_ACTIVE_ENTRY : SPI_STREAM_ENTRY)));
        t->window_open = active;
    }
    t->frame_pending = false;
    t->wait_idle = false;
    t->command_taken = false;
    t->release_polls = 0;
    sm_enable(t->sm, true);
}

static struct spi_command_block *command_block(const struct mclw_spi_target *t) {
    return &table_pool[t->table[0]].command;
}

static uint8_t (*command_pages(const struct mclw_spi_target *t))[MCLW_COMMAND_PAGE_SIZE] {
    return (uint8_t (*)[MCLW_COMMAND_PAGE_SIZE])(void *)&table_pool[t->table[1]];
}

static uint32_t command_ctrl(uint channel, uint chain_to, enum dma_channel_transfer_size size, bool read_increment,
    bool write_increment, uint dreq) {
    dma_channel_config c = dma_channel_get_default_config(channel);

    channel_config_set_transfer_data_size(&c, size);
    channel_config_set_read_increment(&c, read_increment);
    channel_config_set_write_increment(&c, write_increment);
    channel_config_set_dreq(&c, dreq);
    channel_config_set_chain_to(&c, chain_to);
    channel_config_set_high_priority(&c, true);
    channel_config_set_irq_quiet(&c, true);
    return channel_config_get_ctrl_value(&c);
}

static uint32_t address_of(const volatile void *p) {
    return (uint32_t)(uintptr_t)p;
}

// The engine's copy of what the model published. Called with no window open.
static uint8_t __not_in_flash_func(command_shadow)(struct mclw_spi_target *t) {
    struct mclw_command_image *image = t->command;
    struct spi_command_block *b = command_block(t);
    uint8_t dirty = image->dirty;

    image->dirty = 0;
    if ((dirty & MCLW_COMMAND_DIRTY_STATUS) != 0u) {
        b->status = image->status * 0x01010101u;
    }
    if ((dirty & MCLW_COMMAND_DIRTY_RESPONSES) != 0u) {
        for (uint8_t i = 0; i < image->response_count; i++) {
            memcpy(b->responses[i], image->responses[i].bytes, MCLW_COMMAND_RESPONSE_MAX);
        }
    }
    if ((dirty & MCLW_COMMAND_DIRTY_PAGES) != 0u) {
        for (uint8_t i = 0; i < image->page_count; i++) {
            memcpy(command_pages(t)[t->page_slot[i]], image->pages[i], MCLW_COMMAND_PAGE_SIZE);
        }
    }
    if ((dirty & MCLW_COMMAND_DIRTY_BUFFER) != 0u) {
        memcpy(b->buffer, image->buffer, MCLW_COMMAND_BUFFER_SIZE);
        memcpy(b->buffer + MCLW_COMMAND_BUFFER_SIZE, image->buffer, MCLW_COMMAND_BUFFER_SIZE);
    }
    t->shadow_pending = false;
    return dirty;
}

// A register read runs on from its page's slot into the next one, as the
// part's address runs on into the next page, so the slot after a page must
// hold the page after it. Held pages sit in runs of consecutive numbers,
// each run after a zero slot and the last one before another. An unheld
// page maps to the zero slot before the run its successor starts, or else
// to slot 0, which zero slot 1 follows.
static void command_layout(struct mclw_spi_target *t, uint8_t *pagetab) {
    const struct mclw_command_image *image = t->command;
    uint8_t (*pages)[MCLW_COMMAND_PAGE_SIZE] = command_pages(t);
    uint8_t next = 1;
    int previous = -2;

    memset(pagetab, (int)((address_of(pages[0]) >> 8) & 0xffu), 256u);
    for (int page = 0; page < 256; page++) {
        for (uint8_t i = 0; i < image->page_count; i++) {
            if (image->page_number[i] != page) {
                continue;
            }
            if (page != previous + 1) {
                if (page > 0) {
                    pagetab[page - 1] = (uint8_t)((address_of(pages[next]) >> 8) & 0xffu);
                }
                next++;
            }
            t->page_slot[i] = next;
            pagetab[page] = (uint8_t)((address_of(pages[next]) >> 8) & 0xffu);
            next++;
            previous = page;
        }
    }
}

static void command_build(struct mclw_spi_target *t) {
    const struct mclw_command_image *image = t->command;
    struct spi_command_block *b = command_block(t);
    struct spi_cb *l = b->lists;
    struct spi_cb *p = b->prologue;
    uint8_t (*pages)[MCLW_COMMAND_PAGE_SIZE] = command_pages(t);
    uint L = (uint)t->loader_dma;
    uint W = (uint)t->rx_dma;
    uint D = (uint)t->tx_dma;
    uint rx_dreq = pio_get_dreq(SPI_PIO, t->sm, false);
    uint tx_dreq = pio_get_dreq(SPI_PIO, t->sm, true);
    uint32_t rxf = address_of(&SPI_PIO->rxf[t->sm]);
    uint32_t txf = address_of(tx_fifo_msb(t));
    uint32_t d_al1 = address_of(&dma_hw->ch[D].al1_ctrl);
    uint32_t rx_byte = command_ctrl(W, L, DMA_SIZE_8, false, false, rx_dreq);
    uint32_t tx_status = command_ctrl(W, L, DMA_SIZE_8, false, false, tx_dreq);
    uint32_t copy_byte = command_ctrl(W, L, DMA_SIZE_8, false, false, DREQ_FORCE);
    uint32_t copy_word = command_ctrl(W, L, DMA_SIZE_32, false, false, DREQ_FORCE);
    uint32_t jump = command_ctrl(W, W, DMA_SIZE_32, false, false, DREQ_FORCE);
    uint32_t load_d = command_ctrl(W, L, DMA_SIZE_32, true, true, DREQ_FORCE);
    uint32_t drain = command_ctrl(W, L, DMA_SIZE_8, false, true, rx_dreq);
    uint32_t page_high = address_of(pages) & 0xffff0000u;

    memset(b, 0, sizeof(*b));
    memset(pages, 0, SPI_COMMAND_PAGE_SLOTS * MCLW_COMMAND_PAGE_SIZE);

    memset(b->optab, (int)(SPI_LIST_WRITE * sizeof(struct spi_cb)), sizeof(b->optab));
    for (uint8_t i = 0; i < image->response_count; i++) {
        b->optab[image->responses[i].opcode] = (uint8_t)(SPI_LIST_RESPONSE * sizeof(struct spi_cb));
        b->resptab[image->responses[i].opcode] = (uint8_t)(i * MCLW_COMMAND_RESPONSE_MAX);
    }
    b->optab[image->read_buffer_opcode] = (uint8_t)(SPI_LIST_BUFFER * sizeof(struct spi_cb));
    b->optab[image->read_register_opcode] = (uint8_t)(SPI_LIST_REGISTER * sizeof(struct spi_cb));
    command_layout(t, b->pagetab);

    b->dblock[SPI_DBLOCK_WRITE] = (struct spi_dblock){
        command_ctrl(D, D, DMA_SIZE_8, false, false, tx_dreq), address_of(&b->status), txf, SPI_COMMAND_STATUS_RUN };
    b->dblock[SPI_DBLOCK_RESPONSE] = (struct spi_dblock){
        command_ctrl(D, D, DMA_SIZE_8, true, false, tx_dreq), address_of(b->responses), txf,
        MCLW_COMMAND_RESPONSE_MAX };
    b->dblock[SPI_DBLOCK_BUFFER] = (struct spi_dblock){
        command_ctrl(D, D, DMA_SIZE_8, true, false, tx_dreq), address_of(b->buffer), txf, SPI_COMMAND_DATA_RUN };
    b->dblock[SPI_DBLOCK_REGISTER] = (struct spi_dblock){
        command_ctrl(D, D, DMA_SIZE_8, true, false, tx_dreq), page_high, txf, SPI_COMMAND_DATA_RUN };
    b->jump = address_of(b->lists) | SPI_COMMAND_NO_OPCODE;

    struct spi_cb spacer = { address_of(&b->status), address_of(&b->scratch), 1u, copy_word };
    struct spi_cb drain_rest = { rxf, address_of(t->capture), MCLW_COMMAND_WINDOW_MAX, drain };
    uint32_t opcode = address_of(&p[SPI_PROLOGUE_LOOKUP].read);

    p[SPI_PROLOGUE_STATUS] = (struct spi_cb){ address_of(&b->status), txf, 2u, tx_status };
    p[SPI_PROLOGUE_OPCODE] = (struct spi_cb){ rxf, opcode, 1u, rx_byte };
    p[SPI_PROLOGUE_SPACER] = spacer;
    p[SPI_PROLOGUE_LOOKUP] = (struct spi_cb){
        address_of(b->optab), address_of(&b->jump), 1u, copy_byte };
    p[SPI_PROLOGUE_JUMP] = (struct spi_cb){
        address_of(&b->jump), address_of(&dma_hw->ch[L].al3_read_addr_trig), 1u, jump };

    l[SPI_LIST_WRITE + 0] = (struct spi_cb){ address_of(&b->dblock[SPI_DBLOCK_WRITE]), d_al1, 4u, load_d };
    l[SPI_LIST_WRITE + 1] = drain_rest;

    l[SPI_LIST_RESPONSE + 0] = (struct spi_cb){
        opcode, address_of(&l[SPI_LIST_RESPONSE + 1].read), SPI_COMMAND_STEP_REPEAT, copy_byte };
    l[SPI_LIST_RESPONSE + 1] = (struct spi_cb){
        address_of(b->resptab), address_of(&b->dblock[SPI_DBLOCK_RESPONSE].read), 1u, copy_byte };
    l[SPI_LIST_RESPONSE + 2] = (struct spi_cb){ address_of(&b->dblock[SPI_DBLOCK_RESPONSE]), d_al1, 4u, load_d };
    l[SPI_LIST_RESPONSE + 3] = drain_rest;

    l[SPI_LIST_BUFFER + 0] = (struct spi_cb){ address_of(&b->status), txf, 1u, tx_status };
    l[SPI_LIST_BUFFER + 1] = (struct spi_cb){ rxf, address_of(&b->dblock[SPI_DBLOCK_BUFFER].read), 1u, rx_byte };
    l[SPI_LIST_BUFFER + 2] = (struct spi_cb){ address_of(&b->dblock[SPI_DBLOCK_BUFFER]), d_al1, 4u, load_d };
    l[SPI_LIST_BUFFER + 3] = drain_rest;

    l[SPI_LIST_REGISTER + 0] = (struct spi_cb){ address_of(&b->status), txf, 2u, tx_status };
    l[SPI_LIST_REGISTER + 1] = (struct spi_cb){ rxf, address_of(&l[SPI_LIST_REGISTER + 3].read), 1u, rx_byte };
    l[SPI_LIST_REGISTER + 2] = spacer;
    l[SPI_LIST_REGISTER + 3] = (struct spi_cb){
        address_of(b->pagetab), address_of(&b->dblock[SPI_DBLOCK_REGISTER].read) + 1u, 1u, copy_byte };
    l[SPI_LIST_REGISTER + 4] = (struct spi_cb){ rxf, address_of(&b->dblock[SPI_DBLOCK_REGISTER].read), 1u, rx_byte };
    l[SPI_LIST_REGISTER + 5] = (struct spi_cb){ address_of(&b->dblock[SPI_DBLOCK_REGISTER]), d_al1, 4u, load_d };
    l[SPI_LIST_REGISTER + 6] = drain_rest;

    t->command->dirty = 0xffu;
    (void)command_shadow(t);
}

static void __not_in_flash_func(command_prepare)(struct mclw_spi_target *t) {
    struct spi_command_block *b = command_block(t);
    dma_channel_config c = dma_channel_get_default_config((uint)t->loader_dma);

    pio_sm_set_enabled(SPI_PIO, t->sm, false);
    stop_dma(t);
    set_autoshift(t, false, true, 8u, 8u);
    pio_sm_clear_fifos(SPI_PIO, t->sm);
    pio_sm_restart(SPI_PIO, t->sm);
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_mov(pio_isr, pio_null));
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_out(pio_null, 32u));
    miso_release(t);
    *(volatile uint8_t *)&b->jump = SPI_COMMAND_NO_OPCODE;

    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, 4u);
    channel_config_set_high_priority(&c, true);
    channel_config_set_irq_quiet(&c, true);
    __compiler_memory_barrier();
    dma_channel_configure((uint)t->loader_dma, &c, &dma_hw->ch[t->rx_dma].read_addr, b->prologue, 4u, true);

    set_autoshift(t, true, true, 8u, 8u);
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_jmp(t->bus->program_offset + SPI_STREAM_ENTRY));
    t->window_open = false;
    t->frame_pending = false;
    t->wait_idle = false;
    t->command_taken = false;
    t->release_polls = 0;
    pio_sm_set_enabled(SPI_PIO, t->sm, true);
}

// How many control blocks of the list at `head` the loader has fetched.
static uint32_t __not_in_flash_func(command_loaded)(const struct mclw_spi_target *t, uint32_t head) {
    uint32_t start = address_of(&command_block(t)->lists[head]);
    uint32_t at = dma_channel_hw_addr((uint)t->loader_dma)->read_addr;

    return at > start ? (at - start) / sizeof(struct spi_cb) : 0u;
}

static void __not_in_flash_func(command_finalize)(struct mclw_spi_target *t) {
    const volatile struct spi_command_block *b = command_block(t);
    uint8_t list;
    uint32_t remaining;
    uint16_t len = 0;
    bool fault;

    quiesce(t);
    list = *(const volatile uint8_t *)&b->jump;
    remaining = dma_channel_hw_addr((uint)t->rx_dma)->transfer_count;
    fault = dma_faulted(t);
    stop_dma(t);
    if (fault) {
        t->stats.dma_faults++;
    }
    if (t->window_busy || t->record_full) {
        t->stats.stale_windows++;
    }
    // The window before is still waiting for the worker, BUSY high since its
    // release: the part ignores this one, and the one it was busy with is
    // kept for delivery.
    if (t->record_full) {
        return;
    }
    if (list == SPI_COMMAND_NO_OPCODE) {
        t->stats.short_windows++;
    } else {
        uint32_t head = list / sizeof(struct spi_cb);
        uint32_t loaded = command_loaded(t, head);

        t->record[len++] = *(const volatile uint8_t *)&b->prologue[SPI_PROLOGUE_LOOKUP].read;
        if (head == SPI_LIST_WRITE && loaded >= 2u) {
            uint32_t params = remaining <= MCLW_COMMAND_WINDOW_MAX ?
                MCLW_COMMAND_WINDOW_MAX - remaining : 0u;

            if (loaded > 2u) {
                params = MCLW_COMMAND_WINDOW_MAX;
                t->stats.long_windows++;
            }
            memcpy(&t->record[len], t->capture, params);
            len = (uint16_t)(len + params);
        } else if (head == SPI_LIST_BUFFER && loaded >= 3u) {
            t->record[len++] = *(const volatile uint8_t *)&b->dblock[SPI_DBLOCK_BUFFER].read;
        } else if (head == SPI_LIST_REGISTER && loaded >= 3u) {
            t->record[len++] = *(const volatile uint8_t *)&b->lists[SPI_LIST_REGISTER + 3].read;
            if (loaded >= 6u) {
                t->record[len++] = *(const volatile uint8_t *)&b->dblock[SPI_DBLOCK_REGISTER].read;
            }
        }
        t->stats.transactions++;
        t->stats.bytes += len;
    }
    t->record_len = len;
    t->record_busy = t->window_busy;
    t->record_full = true;
    mclw_hal_work_post(&t->record_work);
}

// Whether the state machine is still waiting for chip select, with no
// window begun since the last release.
static bool command_idle(const struct mclw_spi_target *t) {
    uint8_t pc = (uint8_t)(pio_sm_get_pc(SPI_PIO, t->sm) - t->bus->program_offset);

    return pc < SPI_STREAM_ACTIVE_ENTRY && gpio_get(t->cs) &&
        *(volatile uint8_t *)&command_block(t)->jump == SPI_COMMAND_NO_OPCODE;
}

static void command_notify(struct mclw_command_image *image, void *ctx) {
    struct mclw_spi_target *t = ctx;
    uint32_t state = mclw_hal_critical_enter();

    if (t->image == SPI_IMAGE_COMMAND) {
        if (!command_idle(t)) {
            t->shadow_pending = true;
        } else if ((command_shadow(t) & MCLW_COMMAND_DIRTY_STATUS) != 0u && command_idle(t)) {
            // Between windows the state machine holds the first status byte
            // in its OSR and the second in the TX FIFO, both copied at the
            // last release, before the model ran. Both are replaced and the
            // machine re-enters at the top, where it waits for a fresh
            // chip-select assertion. A window the host began during the
            // copy is not cut: it keeps the old status, and its release
            // loads the new one.
            pio_sm_clear_fifos(SPI_PIO, t->sm);
            pio_sm_put(SPI_PIO, t->sm, (uint32_t)image->status << 24);
            pio_sm_put(SPI_PIO, t->sm, (uint32_t)image->status << 24);
            pio_sm_exec(SPI_PIO, t->sm, pio_encode_out(pio_null, 32u));
            pio_sm_exec(SPI_PIO, t->sm, pio_encode_jmp(t->bus->program_offset + SPI_STREAM_ENTRY));
        }
    }
    mclw_hal_critical_exit(state);
}

static void command_deliver(struct mclw_work *work) {
    struct mclw_spi_target *t = work->ctx;
    struct mclw_command_image *image;
    uint32_t state;
    uint16_t len;
    bool busy;

    if (!t->record_full) {
        return;
    }
    if (!mclw_hal_try_lock()) {
        mclw_hal_work_post(work);
        return;
    }
    state = mclw_hal_critical_enter();
    image = t->image == SPI_IMAGE_COMMAND ? t->command : NULL;
    len = t->record_len;
    busy = t->record_busy;
    memcpy(t->delivered, t->record, len);
    t->record_full = false;
    mclw_hal_critical_exit(state);
    // A publish made while this window was open is copied here, under the
    // runtime lock, and not at release: core 0 may be editing the image
    // then. BUSY, still high, keeps the host out until it is.
    if (image != NULL && t->shadow_pending) {
        command_notify(image, t);
    }
    if (image != NULL && image->on_window != NULL) {
        image->on_window(image, t->delivered, len, busy, image->on_window_ctx);
    }
    mclw_hal_unlock();
}

static void __not_in_flash_func(prepare)(struct mclw_spi_target *t, bool join) {
    if (t->image == SPI_IMAGE_REGMAP) {
        regmap_prepare(t, join);
    } else if (t->image == SPI_IMAGE_STREAM) {
        stream_prepare(t, join);
    } else if (t->image == SPI_IMAGE_COMMAND) {
        command_prepare(t);
    }
}

static bool __not_in_flash_func(command_pending)(const struct mclw_spi_target *t) {
    dma_channel_hw_t *rx = dma_channel_hw_addr((uint)t->rx_dma);

    return (rx->ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) != 0u || rx->transfer_count != 0u;
}

static void __not_in_flash_func(service_command)(struct mclw_spi_target *t) {
    dma_channel_hw_t *rx = dma_channel_hw_addr((uint)t->rx_dma);


    dma_irqn_acknowledge_channel(1u, (uint)t->rx_dma);
    dma_channel_set_irq1_enabled((uint)t->rx_dma, false);
    rx->write_addr = (uintptr_t)t->capture;
    rx->transfer_count = SPI_BUFFER_BYTES;
    rx->ctrl_trig = t->payload_ctrl;
    t->command_taken = true;
}

static bool __not_in_flash_func(command_byte)(const struct mclw_spi_target *t, uint8_t *out) {
    uintptr_t base = (uintptr_t)table_pool[t->table[t->bank]].descriptors;
    uintptr_t loaded;
    uintptr_t descriptor;

    for (uint32_t i = 0; i < SPI_LOADER_WAIT_LOOPS; i++) {
        if ((dma_channel_hw_addr((uint)t->loader_dma)->ctrl_trig &
            DMA_CH0_CTRL_TRIG_BUSY_BITS) == 0u) {
            break;
        }
        tight_loop_contents();
    }
    loaded = dma_channel_hw_addr((uint)t->loader_dma)->read_addr;
    if (loaded < base + sizeof(struct spi_descriptor) ||
        loaded > base + SPI_BANK_BYTES * sizeof(struct spi_descriptor) ||
        ((loaded - base) % sizeof(struct spi_descriptor)) != 0u) {
        return false;
    }
    descriptor = loaded - sizeof(struct spi_descriptor);
    *out = (uint8_t)((descriptor - base) / sizeof(struct spi_descriptor));
    return true;
}

static void __not_in_flash_func(regmap_finalize)(struct mclw_spi_target *t) {
    struct mclw_regmap *map = t->map;
    uint32_t remaining;
    uint32_t payload = 0;
    uint32_t available;
    uint32_t address;
    uint32_t dummies = t->framing.read_dummy_bytes;
    uint8_t command = 0;
    bool fault;
    bool have_command;

    if (!t->command_taken && dma_irqn_get_channel_status(1u, (uint)t->rx_dma) && !command_pending(t)) {
        service_command(t);
    }
    if (t->command_taken) {
        quiesce(t);
    }
    remaining = dma_channel_hw_addr((uint)t->rx_dma)->transfer_count;
    fault = dma_faulted(t);
    have_command = t->command_taken && command_byte(t, &command);
    stop_dma(t);

    if (fault) {
        t->stats.dma_faults++;
    }
    if (!have_command) {
        if (t->command_taken && !fault) {
            t->stats.dma_faults++;
        }
        t->stats.short_windows++;
        return;
    }
    if (remaining <= SPI_BUFFER_BYTES) {
        payload = SPI_BUFFER_BYTES - remaining;
    }
    t->stats.transactions++;
    t->stats.bytes += payload + 1u;

    address = command & t->framing.address_mask;
    if ((command & t->framing.read_bit_mask) != 0u) {
        uint32_t served;

        available = dummies + (SPI_BANK_BYTES - address);
        if (payload > available) {
            t->stats.long_windows++;
            payload = available;
        }
        served = payload > dummies ? payload - dummies : 0u;
        mclw_regmap_bus_read_burst(map, (uint8_t)address, served, t->framing.auto_increment);
    } else {
        if (payload >= SPI_BUFFER_BYTES) {
            t->stats.long_windows++;
        }
        if (payload > SPI_BANK_BYTES) {
            payload = SPI_BANK_BYTES;
        }
        mclw_regmap_bus_write_burst(map, (uint8_t)address, t->capture, payload,
            t->framing.auto_increment);
    }
}

static void __not_in_flash_func(stream_finalize)(struct mclw_spi_target *t) {
    struct mclw_stream *s = t->stream;
    uint32_t remaining;
    uint32_t count = 0;
    bool fault;

    quiesce(t);
    remaining = dma_channel_hw_addr((uint)t->rx_dma)->transfer_count;
    fault = dma_faulted(t);
    stop_dma(t);

    if (fault) {
        t->stats.dma_faults++;
    }
    if (remaining <= SPI_BUFFER_BYTES) {
        count = SPI_BUFFER_BYTES - remaining;
    }
    if (count == 0u) {
        t->stats.short_windows++;
        return;
    }
    t->stats.transactions++;
    t->stats.bytes += count;
    if (count < (uint32_t)s->length) {
        t->stats.short_windows++;
    } else if (count > (uint32_t)s->length) {
        t->stats.long_windows++;
    }
    if (t->frame_pending) {
        t->stats.stale_windows++;
    }
    if (s->direction == MCLW_STREAM_IN && s->on_frame != NULL) {
        uint32_t len = count > SPI_FRAME_BYTES ? SPI_FRAME_BYTES : count;

        if (!t->msb_first) {
            for (uint32_t i = 0; i < len; i++) {
                t->capture[i] = reverse_byte(t->capture[i]);
            }
        }
        s->on_frame(s, t->capture, len, s->on_frame_ctx);
    }
}

static void __not_in_flash_func(finalize)(struct mclw_spi_target *t) {
    uint32_t state = mclw_hal_critical_enter();

    if (t->image == SPI_IMAGE_NONE || !t->window_open) {
        mclw_hal_critical_exit(state);
        return;
    }
    t->window_open = false;
    miso_release(t);
    if (t->image == SPI_IMAGE_REGMAP) {
        regmap_finalize(t);
    } else if (t->image == SPI_IMAGE_STREAM) {
        stream_finalize(t);
    } else {
        command_finalize(t);
    }
    prepare(t, true);
    t->last_release_us = time_us_32();
    t->released = true;
    mclw_hal_critical_exit(state);
}

static bool __not_in_flash_func(window_evidence)(const struct mclw_spi_target *t) {
    if (t->image == SPI_IMAGE_REGMAP) {
        return t->command_taken || dma_irqn_get_channel_status(1u, (uint)t->rx_dma);
    }
    if (t->image == SPI_IMAGE_STREAM) {
        return dma_channel_hw_addr((uint)t->rx_dma)->transfer_count != SPI_BUFFER_BYTES;
    }
    // Every release is a window, even a bare chip-select pulse: hosts wake
    // a sleeping part with one.
    return t->image == SPI_IMAGE_COMMAND;
}

static void __not_in_flash_func(spi_target_dma_handler)(void) {
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct mclw_spi_target *t = &targets[i];

        if (t->image != SPI_IMAGE_REGMAP || t->rx_dma < 0) {
            continue;
        }
        if (!dma_irqn_get_channel_status(1u, (uint)t->rx_dma)) {
            continue;
        }
        if (t->command_taken || command_pending(t)) {
            dma_irqn_acknowledge_channel(1u, (uint)t->rx_dma);
            continue;
        }
        service_command(t);
    }
    mclw_hal_critical_exit(state);
}

static void __not_in_flash_func(spi_target_release_handler)(void) {
    uint32_t state = mclw_hal_critical_enter();
    uint32_t flags = SPI_PIO->irq;

    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct mclw_spi_target *t = &targets[i];

        if (t->owner == NULL || (flags & (1u << t->monitor_sm)) == 0u) {
            continue;
        }
        pio_interrupt_clear(SPI_PIO, t->monitor_sm);
        if (t->image == SPI_IMAGE_NONE) {
            continue;
        }
        if (t->image == SPI_IMAGE_COMMAND) {
            t->window_busy = t->busy;
            t->busy = t->busy || t->busy_pin >= 0;
        }
        if (t->wait_idle) {
            t->wait_idle = false;
            pio_sm_exec(SPI_PIO, t->sm, pio_encode_jmp(t->bus->program_offset + SPI_REGMAP_ENTRY));
            sm_enable(t->sm, true);
            continue;
        }
        if (!t->window_open && window_evidence(t)) {
            t->window_open = true;
        }
        if (t->window_open) {
            finalize(t);
        }
    }
    mclw_hal_critical_exit(state);
}

// Whether re-arming the stream now could cut a window: one is open or has
// finished unfinalized, chip select is low, or one ended a moment ago and
// the next is likely about to start.
static bool __not_in_flash_func(stream_busy)(const struct mclw_spi_target *t) {
    return t->window_open || !gpio_get(t->cs) || window_evidence(t) ||
        (t->released && (uint32_t)(time_us_32() - t->last_release_us) < SPI_STREAM_BURST_US);
}

static void __not_in_flash_func(stream_notify)(struct mclw_stream *s, void *ctx) {
    struct mclw_spi_target *t = ctx;
    uint32_t state = mclw_hal_critical_enter();

    (void)s;
    if (t->image != SPI_IMAGE_STREAM) {
        mclw_hal_critical_exit(state);
        return;
    }
    if (stream_busy(t)) {
        t->frame_pending = true;
    } else {
        stream_prepare(t, false);
    }
    mclw_hal_critical_exit(state);
}

static void release_irq_install(void *ctx) {
    bool want = *(const bool *)ctx;

    if (want) {
        irq_set_exclusive_handler(PIO1_IRQ_1, spi_target_release_handler);
        irq_set_priority(PIO1_IRQ_1, 0);
        irq_set_enabled(PIO1_IRQ_1, true);
    } else {
        irq_set_enabled(PIO1_IRQ_1, false);
        irq_remove_handler(PIO1_IRQ_1, spi_target_release_handler);
    }
}

static void release_irq_sync(struct spi_bus *bus, bool want) {
    if (want == bus->release_installed) {
        return;
    }
    mclw_hal_work_call(release_irq_install, &want);
    bus->release_installed = want;
}

static void dma_handler_install(void *ctx) {
    bool want = *(const bool *)ctx;

    if (want) {
        irq_add_shared_handler(DMA_IRQ_1, spi_target_dma_handler,
            PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_priority(DMA_IRQ_1, 0);
        irq_set_enabled(DMA_IRQ_1, true);
    } else {
        irq_remove_handler(DMA_IRQ_1, spi_target_dma_handler);
    }
}

static void dma_handler_sync(struct spi_bus *bus, bool want) {
    if (want == bus->dma_installed) {
        return;
    }
    mclw_hal_work_call(dma_handler_install, &want);
    bus->dma_installed = want;
}

static void poll_targets(void *ctx) {
    (void)ctx;
    mclw_hal_spi_target_poll();
}

static void release_tables(struct mclw_spi_target *t) {
    uint8_t *table_owner = t->bus->table_owner;
    uint8_t tag = (uint8_t)((t - targets) + 1);

    for (size_t i = 0; i < SPI_TABLE_SLOTS; i++) {
        if (table_owner[i] == tag) {
            table_owner[i] = 0;
        }
    }
    t->banks = 0;
}

static bool claim_tables(struct mclw_spi_target *t, uint8_t banks) {
    uint8_t *table_owner = t->bus->table_owner;
    uint8_t tag = (uint8_t)((t - targets) + 1);
    uint8_t taken = 0;

    for (size_t i = 0; i < SPI_TABLE_SLOTS && taken < banks; i++) {
        if (table_owner[i] == 0u) {
            table_owner[i] = tag;
            t->table[taken] = (uint8_t)i;
            taken++;
        }
    }
    if (taken < banks) {
        release_tables(t);
        return false;
    }
    t->banks = banks;
    return true;
}

// The command image's block and, for its pages, two adjacent slots within
// one 64 KiB region: the register list composes a page's address from its
// bits 15..8.
static bool claim_command_tables(struct mclw_spi_target *t) {
    uint8_t *table_owner = t->bus->table_owner;
    uint8_t tag = (uint8_t)((t - targets) + 1);

    for (uint8_t i = 0; i + 1u < SPI_TABLE_SLOTS; i++) {
        if (table_owner[i] != 0u || table_owner[i + 1u] != 0u ||
            (address_of(&table_pool[i]) >> 16) != (address_of(&table_pool[i + 2u]) - 1u) >> 16) {
            continue;
        }
        table_owner[i] = tag;
        table_owner[i + 1u] = tag;
        if (!claim_tables(t, 1u)) {
            return false;
        }
        t->table[1] = i;
        t->table[2] = (uint8_t)(i + 1u);
        t->banks = 3;
        return true;
    }
    return false;
}

static void build_tables(struct mclw_spi_target *t) {
    const struct mclw_regmap *map = t->map;

    for (uint8_t b = 0; b < t->banks; b++) {
        struct spi_descriptor *table = table_pool[t->table[b]].descriptors;
        const uint8_t *base = map->value[b];

        for (uint32_t command = 0; command < SPI_BANK_BYTES; command++) {
            if ((command & t->framing.read_bit_mask) != 0u) {
                uint32_t address = command & t->framing.address_mask;

                table[command].count = SPI_BANK_BYTES - address;
                table[command].source = (uint32_t)(uintptr_t)(base + address);
            } else {
                table[command].count = SPI_BUFFER_BYTES;
                table[command].source = (uint32_t)(uintptr_t)t->tx_buf;
            }
        }
    }
}

// SCK, MOSI and MISO are the bus, shared by every target on it, so the
// engine owns them; only the chip select belongs to the instance.
static int bus_claim_pins(struct spi_bus *bus, uint8_t sck, uint8_t mosi, uint8_t miso) {
    const uint8_t pins[3] = { sck, mosi, miso };
    size_t claimed;

    for (claimed = 0; claimed < 3u; claimed++) {
        int err = mclw_hal_pin_claim(pins[claimed], bus);

        if (err != 0) {
            while (claimed > 0u) {
                claimed--;
                mclw_hal_pin_release(pins[claimed], bus);
            }
            return err;
        }
    }
    bus->sck = sck;
    bus->mosi = mosi;
    bus->miso = miso;
    return 0;
}

static void bus_release_pins(struct spi_bus *bus) {
    const uint8_t pins[3] = { bus->sck, bus->mosi, bus->miso };

    hw_clear_bits(&SPI_PIO->input_sync_bypass, 1u << bus->sck);
    for (size_t i = 0; i < 3u; i++) {
        gpio_set_function(pins[i], GPIO_FUNC_SIO);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_disable_pulls(pins[i]);
        mclw_hal_pin_release(pins[i], bus);
    }
}

static void release_cs(const struct mclw_spi_target *t) {
    gpio_set_function(t->cs, GPIO_FUNC_SIO);
    gpio_set_dir(t->cs, GPIO_IN);
    gpio_disable_pulls(t->cs);
    mclw_hal_pin_release(t->cs, t->owner);
}

static bool bus_class_conflict(const struct spi_bus *bus, const struct mclw_spi_target *t,
    uint8_t cls) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        const struct mclw_spi_target *o = &targets[i];

        if (o != t && o->bus == bus && mode_class(o->mode) != cls) {
            return true;
        }
    }
    return false;
}

static void bus_set_class(struct spi_bus *bus, uint8_t cls) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct mclw_spi_target *o = &targets[i];

        if (o->bus == bus) {
            pio_sm_set_enabled(SPI_PIO, o->sm, false);
            stop_dma(o);
            o->window_open = false;
        }
    }
    program_patch(bus, bus->program_sck, cls);
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct mclw_spi_target *o = &targets[i];

        if (o->bus == bus && o->image != SPI_IMAGE_NONE) {
            prepare(o, false);
        }
    }
}

static bool bus_dummy_conflict(const struct spi_bus *bus, const struct mclw_spi_target *t,
    uint16_t dummy) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        const struct mclw_spi_target *o = &targets[i];

        if (o != t && o->bus == bus && o->image == SPI_IMAGE_REGMAP &&
            dummy_instruction(&o->framing, o->map->filler) != dummy) {
            return true;
        }
    }
    return false;
}

struct mclw_spi_target *mclw_hal_spi_target_open(uint8_t sck, uint8_t mosi, uint8_t miso,
    uint8_t cs, uint8_t mode, bool msb_first, const void *owner, int *err) {
    struct spi_bus *bus = &spi_bus;
    struct mclw_spi_target *t = NULL;
    bool claimed_bus = false;
    uint8_t cls;
    int sm;

    if (err == NULL) {
        return NULL;
    }
    if (owner == NULL || mode > 3 || sck == mosi || sck == miso || sck == cs ||
        mosi == miso || mosi == cs || miso == cs) {
        *err = -EINVAL;
        return NULL;
    }
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        if (targets[i].owner == NULL) {
            t = &targets[i];
            break;
        }
    }
    if (t == NULL) {
        *err = -EBUSY;
        return NULL;
    }
    cls = mode_class(mode);
    if (bus->open_count != 0u && (sck != bus->sck || mosi != bus->mosi || miso != bus->miso ||
        cls != bus->program_class)) {
        *err = -ENOSPC;
        return NULL;
    }

    if (bus->open_count == 0u) {
        *err = bus_claim_pins(bus, sck, mosi, miso);
        if (*err != 0) {
            return NULL;
        }
        claimed_bus = true;
    }
    *err = mclw_hal_pin_claim(cs, owner);
    if (*err != 0) {
        if (claimed_bus) {
            bus_release_pins(bus);
        }
        return NULL;
    }

    sm = pio_claim_unused_sm(SPI_PIO, false);
    if (sm < 0) {
        *err = -EBUSY;
        goto fail_pins;
    }
    t->sm = (uint8_t)sm;
    sm = pio_claim_unused_sm(SPI_PIO, false);
    if (sm < 0) {
        pio_sm_unclaim(SPI_PIO, t->sm);
        *err = -EBUSY;
        goto fail_pins;
    }
    t->monitor_sm = (uint8_t)sm;
    t->tx_dma = (int8_t)dma_claim_unused_channel(false);
    t->rx_dma = (int8_t)dma_claim_unused_channel(false);
    t->loader_dma = (int8_t)dma_claim_unused_channel(false);
    if (t->tx_dma < 0 || t->rx_dma < 0 || t->loader_dma < 0) {
        *err = -ENOMEM;
        goto fail_dma;
    }
    *err = bus_load_program(bus, sck, cls);
    if (*err != 0) {
        goto fail_dma;
    }

    t->bus = bus;
    t->cs = cs;
    t->mode = mode;
    t->msb_first = msb_first;
    t->image = SPI_IMAGE_NONE;
    t->map = NULL;
    t->stream = NULL;
    t->command = NULL;
    t->busy_pin = -1;
    t->busy = false;
    t->record_full = false;
    t->shadow_pending = false;
    t->record_work = (struct mclw_work){ .fn = command_deliver, .ctx = t, .pending = false, .next = NULL };
    t->banks = 0;
    t->bank = 0;
    t->window_open = false;
    t->command_taken = false;
    t->wait_idle = false;
    t->frame_pending = false;
    t->release_polls = 0;
    memset(&t->stats, 0, sizeof(t->stats));

    pio_gpio_init(SPI_PIO, sck);
    pio_gpio_init(SPI_PIO, mosi);
    pio_gpio_init(SPI_PIO, miso);
    gpio_set_function(cs, GPIO_FUNC_SIO);
    gpio_set_dir(cs, GPIO_IN);
    gpio_pull_up(cs);
    pio_sm_set_consecutive_pindirs(SPI_PIO, t->sm, miso, 1u, false);
    pio_sm_set_consecutive_pindirs(SPI_PIO, t->sm, sck, 1u, false);
    pio_sm_set_consecutive_pindirs(SPI_PIO, t->sm, mosi, 1u, false);
    {
        pio_sm_config c = pio_get_default_sm_config();

        sm_config_set_in_pins(&c, cs);
        sm_config_set_wrap(&c, bus->program_offset + SPI_MONITOR_ENTRY,
            bus->program_offset + SPI_MONITOR_WRAP_TOP);
        pio_sm_init(SPI_PIO, t->monitor_sm, bus->program_offset + SPI_MONITOR_ENTRY, &c);
        pio_interrupt_clear(SPI_PIO, t->monitor_sm);
        pio_set_irq1_source_enabled(SPI_PIO, (enum pio_interrupt_source)(pis_interrupt0 + t->monitor_sm), true);
        pio_sm_set_enabled(SPI_PIO, t->monitor_sm, true);
    }

    t->owner = owner;
    if (bus->open_count++ == 0u && !bus->poll_registered) {
        bus->poll_registered = mclw_hal_poll_register(poll_targets, NULL) == 0;
    }
    release_irq_sync(bus, true);
    dma_handler_sync(bus, true);
    *err = 0;
    return t;

fail_dma:
    if (t->loader_dma >= 0) {
        dma_channel_unclaim((uint)t->loader_dma);
    }
    if (t->rx_dma >= 0) {
        dma_channel_unclaim((uint)t->rx_dma);
    }
    if (t->tx_dma >= 0) {
        dma_channel_unclaim((uint)t->tx_dma);
    }
    t->loader_dma = -1;
    t->rx_dma = -1;
    t->tx_dma = -1;
    pio_sm_unclaim(SPI_PIO, t->sm);
    pio_sm_unclaim(SPI_PIO, t->monitor_sm);
fail_pins:
    if (claimed_bus) {
        bus_release_pins(bus);
    }
    mclw_hal_pin_release(cs, owner);
    return NULL;
}

void mclw_hal_spi_target_disarm(struct mclw_spi_target *t) {
    uint32_t state;

    if (t == NULL || t->owner == NULL) {
        return;
    }
    state = mclw_hal_critical_enter();
    if (t->image != SPI_IMAGE_NONE) {
        sm_enable(t->sm, false);
        stop_dma(t);
        t->window_open = false;
        t->wait_idle = false;
        t->command_taken = false;
        t->frame_pending = false;
        miso_release(t);
        if (t->stream != NULL) {
            t->stream->notify = NULL;
            t->stream->notify_ctx = NULL;
        }
        if (t->command != NULL) {
            t->command->notify = NULL;
            t->command->notify_ctx = NULL;
        }
        if (t->busy_pin >= 0) {
            pio_sm_set_set_pins(SPI_PIO, t->monitor_sm, 0u, 0u);
            gpio_set_function((uint)t->busy_pin, GPIO_FUNC_SIO);
            t->busy_pin = -1;
        }
        release_tables(t);
        t->image = SPI_IMAGE_NONE;
        t->map = NULL;
        t->stream = NULL;
        t->command = NULL;
        t->busy = false;
        t->record_full = false;
        t->shadow_pending = false;
    }
    mclw_hal_critical_exit(state);
}

void mclw_hal_spi_target_close(struct mclw_spi_target *t) {
    struct spi_bus *bus;

    if (t == NULL || t->owner == NULL) {
        return;
    }
    bus = t->bus;
    mclw_hal_spi_target_disarm(t);

    sm_enable(t->sm, false);
    pio_sm_unclaim(SPI_PIO, t->sm);
    pio_set_irq1_source_enabled(SPI_PIO, (enum pio_interrupt_source)(pis_interrupt0 + t->monitor_sm), false);
    pio_sm_set_enabled(SPI_PIO, t->monitor_sm, false);
    pio_interrupt_clear(SPI_PIO, t->monitor_sm);
    pio_sm_unclaim(SPI_PIO, t->monitor_sm);
    if (t->tx_dma >= 0) {
        dma_channel_unclaim((uint)t->tx_dma);
        t->tx_dma = -1;
    }
    if (t->rx_dma >= 0) {
        dma_channel_unclaim((uint)t->rx_dma);
        t->rx_dma = -1;
    }
    if (t->loader_dma >= 0) {
        dma_channel_unclaim((uint)t->loader_dma);
        t->loader_dma = -1;
    }
    if (bus->open_count == 1u) {
        bus_release_pins(bus);
    }
    release_cs(t);
    t->owner = NULL;
    t->bus = NULL;
    bus->open_count--;
    if (bus->open_count == 0u) {
        release_irq_sync(bus, false);
        dma_handler_sync(bus, false);
        bus_unload_program(bus);
    }
}

int mclw_hal_spi_target_set_mode(struct mclw_spi_target *t, uint8_t mode) {
    struct spi_bus *bus;
    uint8_t cls;
    uint32_t state;

    if (t == NULL || t->owner == NULL || mode > 3) {
        return -EINVAL;
    }
    bus = t->bus;
    cls = mode_class(mode);
    if (cls != bus->program_class && bus_class_conflict(bus, t, cls)) {
        return -ENOSPC;
    }
    state = mclw_hal_critical_enter();
    t->mode = mode;
    if (cls != bus->program_class) {
        bus_set_class(bus, cls);
    } else if (t->image != SPI_IMAGE_NONE) {
        sm_enable(t->sm, false);
        stop_dma(t);
        t->window_open = false;
        prepare(t, false);
    }
    mclw_hal_critical_exit(state);
    return 0;
}

int mclw_hal_spi_target_arm_regmap(struct mclw_spi_target *t, struct mclw_regmap *map,
    const struct mclw_regmap_framing *framing) {
    uint8_t banks;
    uint32_t state;
    uint16_t dummy;

    if (t == NULL || t->owner == NULL || map == NULL || framing == NULL) {
        return -EINVAL;
    }
    if (!t->msb_first) {
        return -ENOTSUP;
    }
    if (framing->read_dummy_bytes > 1u) {
        return -ENOTSUP;
    }
    if (framing->read_dummy_bytes != 0u && map->filler != 0x00u && map->filler != 0xffu) {
        return -ENOTSUP;
    }
    if (!framing->auto_increment && framing->read_dummy_bytes != 0u) {
        return -ENOTSUP;
    }
    dummy = dummy_instruction(framing, map->filler);
    if (bus_dummy_conflict(t->bus, t, dummy)) {
        return -ENOSPC;
    }
    mclw_hal_spi_target_disarm(t);
    banks = map->bank_count == 0 ? 1 : map->bank_count;
    if (banks > MCLW_REGMAP_BANKS_MAX) {
        banks = MCLW_REGMAP_BANKS_MAX;
    }
    state = mclw_hal_critical_enter();
    if (!claim_tables(t, banks)) {
        mclw_hal_critical_exit(state);
        return -ENOMEM;
    }
    t->map = map;
    t->framing = *framing;
    t->image = SPI_IMAGE_REGMAP;
    memset(t->tx_buf, map->filler, sizeof(t->tx_buf));
    build_tables(t);
    memset(&t->stats, 0, sizeof(t->stats));
    if (dummy != t->bus->program_dummy) {
        program_patch_dummy(t->bus, dummy);
    }
    configure_sm(t, SPI_IMAGE_REGMAP);
    pio_interrupt_clear(SPI_PIO, t->monitor_sm);
    regmap_prepare(t, false);
    mclw_hal_critical_exit(state);
    return 0;
}

int mclw_hal_spi_target_arm_stream(struct mclw_spi_target *t,
    struct mclw_stream *s) {
    uint32_t state;

    if (t == NULL || t->owner == NULL || s == NULL) {
        return -EINVAL;
    }
    if (s->length > MCLW_STREAM_FRAME_MAX) {
        return -EINVAL;
    }
    mclw_hal_spi_target_disarm(t);
    state = mclw_hal_critical_enter();
    t->stream = s;
    t->image = SPI_IMAGE_STREAM;
    t->frame_pending = false;
    t->released = false;
    memset(&t->stats, 0, sizeof(t->stats));
    s->notify = stream_notify;
    s->notify_ctx = t;
    configure_sm(t, SPI_IMAGE_STREAM);
    memset(t->tx_buf, wire_byte(t, s->filler), sizeof(t->tx_buf));
    {
        dma_channel_config c = dma_channel_get_default_config((uint)t->tx_dma);

        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_high_priority(&c, true);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(SPI_PIO, t->sm, true));
        t->tx_ctrl = channel_config_get_ctrl_value(&c);
    }
    arm_capture(t, false);
    pio_interrupt_clear(SPI_PIO, t->monitor_sm);
    stream_prepare(t, false);
    mclw_hal_critical_exit(state);
    return 0;
}

int mclw_hal_spi_target_arm_command(struct mclw_spi_target *t, struct mclw_command_image *image,
    struct mclw_gpio *busy) {
    uint32_t state;

    if (t == NULL || t->owner == NULL || image == NULL) {
        return -EINVAL;
    }
    if (!t->msb_first) {
        return -ENOTSUP;
    }
    if (image->page_count > MCLW_COMMAND_PAGES_MAX ||
        image->response_count > MCLW_COMMAND_RESPONSES_MAX) {
        return -EINVAL;
    }
    mclw_hal_spi_target_disarm(t);
    state = mclw_hal_critical_enter();
    if (!claim_command_tables(t)) {
        mclw_hal_critical_exit(state);
        return -ENOMEM;
    }
    t->command = image;
    t->image = SPI_IMAGE_COMMAND;
    t->record_full = false;
    t->shadow_pending = false;
    memset(&t->stats, 0, sizeof(t->stats));
    command_build(t);
    if (busy != NULL) {
        uint8_t pin = mclw_hal_gpio_pin(busy);

        t->busy_pin = (int8_t)pin;
        pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_nop());
        pio_sm_set_set_pins(SPI_PIO, t->monitor_sm, pin, 1u);
        pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_set(pio_pins, 1u));
        pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_set(pio_pindirs, 1u));
        pio_gpio_init(SPI_PIO, pin);
        t->busy = true;
    }
    image->notify = command_notify;
    image->notify_ctx = t;
    configure_sm(t, SPI_IMAGE_COMMAND);
    pio_interrupt_clear(SPI_PIO, t->monitor_sm);
    command_prepare(t);
    mclw_hal_critical_exit(state);
    return 0;
}

void mclw_hal_spi_target_set_busy(struct mclw_spi_target *t, bool busy) {
    uint32_t state;

    if (t == NULL || t->owner == NULL) {
        return;
    }
    state = mclw_hal_critical_enter();
    if (t->image == SPI_IMAGE_COMMAND && t->busy_pin >= 0) {
        pio_sm_exec(SPI_PIO, t->monitor_sm, pio_encode_set(pio_pins, busy ? 1u : 0u));
        t->busy = busy;
    }
    mclw_hal_critical_exit(state);
}

void mclw_hal_spi_target_poll(void) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct mclw_spi_target *t = &targets[i];

        // A command window is only ever closed by the release monitor: it
        // must be closed exactly once, and the BUSY line it raises is the
        // host's guarantee that nothing else happens in between.
        if (t->owner == NULL || t->image == SPI_IMAGE_NONE || t->image == SPI_IMAGE_COMMAND) {
            continue;
        }
        if (t->wait_idle) {
            if (gpio_get(t->cs)) {
                uint32_t state = mclw_hal_critical_enter();

                if (t->wait_idle && gpio_get(t->cs)) {
                    t->wait_idle = false;
                    pio_sm_exec(SPI_PIO, t->sm,
                        pio_encode_jmp(t->bus->program_offset + SPI_REGMAP_ENTRY));
                    sm_enable(t->sm, true);
                }
                mclw_hal_critical_exit(state);
            }
            continue;
        }
        if (t->image == SPI_IMAGE_STREAM && t->frame_pending) {
            // A frame published in a burst waits for the next window end; if
            // none comes, arm it once the engine has been idle long enough.
            uint32_t state = mclw_hal_critical_enter();

            if (t->image == SPI_IMAGE_STREAM && t->frame_pending && !stream_busy(t)) {
                stream_prepare(t, false);
            }
            mclw_hal_critical_exit(state);
        }
        if (!t->window_open) {
            continue;
        }
        if (!gpio_get(t->cs)) {
            t->release_polls = 0;
            continue;
        }
        if (t->release_polls < SPI_RELEASE_POLLS) {
            t->release_polls++;
            continue;
        }
        busy_wait_us_32(SPI_RELEASE_SETTLE_US);
        if (gpio_get(t->cs)) {
            finalize(t);
        }
    }
}

void mclw_hal_spi_target_stats(const struct mclw_spi_target *t,
    struct mclw_spi_target_stats *out) {
    uint32_t state;

    if (out == NULL) {
        return;
    }
    if (t == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    state = mclw_hal_critical_enter();
    *out = t->stats;
    mclw_hal_critical_exit(state);
}

uint8_t mclw_hal_spi_target_mode(const struct mclw_spi_target *t) {
    if (t == NULL) {
        return 0u;
    }
    return t->mode;
}

uint32_t mclw_hal_spi_target_max_clock_hz(const struct mclw_spi_target *t) {
    if (t == NULL) {
        return 0u;
    }
    switch (t->image) {
        case SPI_IMAGE_REGMAP:
            return SPI_REGMAP_CLOCK_HZ;
        case SPI_IMAGE_STREAM:
            return SPI_STREAM_CLOCK_HZ;
        case SPI_IMAGE_COMMAND:
            return SPI_COMMAND_CLOCK_HZ;
        case SPI_IMAGE_NONE:
        default:
            return 0u;
    }
}
