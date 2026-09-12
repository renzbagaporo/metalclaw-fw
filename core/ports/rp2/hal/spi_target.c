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
#include "pico/time.h"

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
#define SPI_MONITOR_ENTRY 29u
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
#define SPI_BANK_BYTES ((uint32_t)METALCLAW_REGMAP_BANK_SIZE)
#define SPI_FRAME_BYTES ((uint32_t)METALCLAW_STREAM_FRAME_MAX)

#define SPI_REGMAP_CLOCK_HZ 10714285u
#define SPI_STREAM_CLOCK_HZ 15000000u

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
};

struct spi_descriptor {
    uint32_t count;
    uint32_t source;
};

struct metalclaw_spi_target {
    const void *owner;
    uint8_t sck;
    uint8_t mosi;
    uint8_t miso;
    uint8_t cs;
    uint8_t mode;
    bool msb_first;
    uint8_t sm;
    uint8_t monitor_sm;
    int8_t tx_dma;
    int8_t rx_dma;
    int8_t loader_dma;

    enum spi_image image;
    struct metalclaw_regmap *map;
    struct metalclaw_regmap_framing framing;
    struct metalclaw_stream *stream;

    uint8_t banks;
    uint8_t table[METALCLAW_REGMAP_BANKS_MAX];
    uint8_t bank;

    volatile bool window_open;
    volatile bool command_taken;
    volatile bool wait_idle;
    volatile bool frame_pending;
    uint8_t release_polls;
    uint32_t payload_ctrl;
    uint32_t tx_ctrl;

    uint8_t tx_buf[SPI_BUFFER_BYTES];
    uint8_t capture[SPI_BUFFER_BYTES];

    struct metalclaw_spi_target_stats stats;
};

static struct metalclaw_spi_target targets[SPI_TARGET_COUNT];

static struct spi_descriptor table_pool[SPI_TABLE_SLOTS][METALCLAW_REGMAP_BANK_SIZE]
    __attribute__((aligned(SPI_TABLE_ALIGN)));
static uint8_t table_owner[SPI_TABLE_SLOTS];

static uint16_t program_words[SPI_PROGRAM_LENGTH];
static pio_program_t spi_program;
static bool program_loaded;
static uint8_t program_offset;
static uint8_t program_sck;
static uint8_t program_class;
static uint16_t program_dummy;
static uint8_t open_count;
static bool release_installed;
static bool dma_installed;

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

static uint8_t __not_in_flash_func(wire_byte)(const struct metalclaw_spi_target *t, uint8_t value) {
    return t->msb_first ? reverse_byte(value) : value;
}

static void encode_program(uint8_t sck, uint8_t cls) {
    uint16_t *p = program_words;
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
    p[25] = program_dummy;
    p[26] = (uint16_t)pio_encode_out(pio_pins, 1u);
    p[27] = (uint16_t)pio_encode_jmp(5u);
    p[28] = (uint16_t)pio_encode_nop();
    p[29] = (uint16_t)pio_encode_wait_pin(false, 0u);
    p[30] = (uint16_t)pio_encode_wait_pin(true, 0u);
    p[31] = (uint16_t)pio_encode_irq_set(true, 0u);

    spi_program.instructions = program_words;
    spi_program.length = (uint8_t)SPI_PROGRAM_LENGTH;
    spi_program.origin = -1;
    spi_program.pio_version = 0;
}

static uint16_t dummy_instruction(const struct metalclaw_regmap_framing *framing, uint8_t filler) {
    if (framing->read_dummy_bytes == 0u) {
        return (uint16_t)pio_encode_nop();
    }
    return filler == 0u ? (uint16_t)pio_encode_mov(pio_osr, pio_null)
        : (uint16_t)pio_encode_mov_not(pio_osr, pio_null);
}

static void program_patch_dummy(uint16_t instruction) {
    program_dummy = instruction;
    program_words[SPI_DUMMY_SLOT] = instruction;
    SPI_PIO->instr_mem[program_offset + SPI_DUMMY_SLOT] = instruction;
}

static void program_patch(uint8_t sck, uint8_t cls) {
    encode_program(sck, cls);
    for (size_t i = 0; i < sizeof(wait_slots); i++) {
        uint8_t slot = wait_slots[i];

        SPI_PIO->instr_mem[program_offset + slot] = program_words[slot];
    }
    program_sck = sck;
    program_class = cls;
}

static bool __not_in_flash_func(miso_held_elsewhere)(const struct metalclaw_spi_target *t) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        const struct metalclaw_spi_target *o = &targets[i];

        if (o != t && o->owner != NULL && o->miso == t->miso && o->window_open) {
            return true;
        }
    }
    return false;
}

static void __not_in_flash_func(miso_release)(const struct metalclaw_spi_target *t) {
    if (miso_held_elsewhere(t)) {
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

static void __not_in_flash_func(stop_dma)(struct metalclaw_spi_target *t) {
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

static bool __not_in_flash_func(dma_faulted)(const struct metalclaw_spi_target *t) {
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

static void __not_in_flash_func(quiesce)(const struct metalclaw_spi_target *t) {
    uint32_t previous;
    uint32_t stable = 0;

    if (t->rx_dma < 0) {
        return;
    }
    previous = dma_channel_hw_addr((uint)t->rx_dma)->transfer_count;
    busy_wait_at_least_cycles(32);
    {
        uint8_t pc = pio_sm_get_pc(SPI_PIO, t->sm);
        bool rest = pc == (uint8_t)(program_offset + SPI_REST_LEAD) ||
            pc == (uint8_t)(program_offset + SPI_REST_SAMPLE) ||
            pc == (uint8_t)(program_offset + SPI_REST_SHIFT) ||
            pc == (uint8_t)(program_offset + SPI_REST_FIRST);

        if (rest && pio_sm_is_rx_fifo_empty(SPI_PIO, t->sm) &&
            dma_channel_hw_addr((uint)t->rx_dma)->transfer_count == previous) {
            return;
        }
    }
    for (uint32_t i = 0; i < SPI_RELEASE_BUDGET_US; i++) {
        uint32_t remaining;
        uint8_t pc;
        bool rest;

        busy_wait_us_32(1u);
        remaining = dma_channel_hw_addr((uint)t->rx_dma)->transfer_count;
        pc = pio_sm_get_pc(SPI_PIO, t->sm);
        rest = pc == (uint8_t)(program_offset + SPI_REST_LEAD) ||
            pc == (uint8_t)(program_offset + SPI_REST_SAMPLE) ||
            pc == (uint8_t)(program_offset + SPI_REST_SHIFT) ||
            pc == (uint8_t)(program_offset + SPI_REST_FIRST);
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

static void configure_sm(struct metalclaw_spi_target *t, bool regmap) {
    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_out_pins(&c, t->miso, 1u);
    sm_config_set_set_pins(&c, t->miso, 1u);
    sm_config_set_in_pins(&c, t->mosi);
    sm_config_set_jmp_pin(&c, t->cs);
    if (regmap) {
        sm_config_set_out_shift(&c, false, false, 8u);
        sm_config_set_in_shift(&c, false, false, 32u);
        sm_config_set_wrap(&c, program_offset + SPI_REGMAP_WRAP_TARGET,
            program_offset + SPI_REGMAP_WRAP_TOP);
        pio_sm_init(SPI_PIO, t->sm, program_offset + SPI_REGMAP_ENTRY, &c);
    } else {
        sm_config_set_out_shift(&c, true, false, 8u);
        sm_config_set_in_shift(&c, false, true, 8u);
        sm_config_set_wrap(&c, program_offset + SPI_STREAM_WRAP_TARGET,
            program_offset + SPI_STREAM_WRAP_TOP);
        pio_sm_init(SPI_PIO, t->sm, program_offset + SPI_STREAM_ENTRY, &c);
    }
    pio_sm_set_consecutive_pindirs(SPI_PIO, t->sm, t->miso, 1u, false);
}

static void __not_in_flash_func(set_autoshift)(const struct metalclaw_spi_target *t, bool pull, bool push,
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

static volatile void *__not_in_flash_func(tx_fifo_msb)(const struct metalclaw_spi_target *t) {
    return (volatile uint8_t *)&SPI_PIO->txf[t->sm] + 3;
}

static void __not_in_flash_func(arm_capture)(struct metalclaw_spi_target *t, bool trigger) {
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

static void __not_in_flash_func(regmap_prepare)(struct metalclaw_spi_target *t, bool join) {
    struct metalclaw_regmap *map = t->map;
    const struct spi_descriptor *table;
    dma_channel_config c;
    uint8_t bank = map->active_bank;
    bool idle_wait;
    bool active;

    if (bank >= t->banks) {
        bank = 0;
    }
    t->bank = bank;
    table = table_pool[t->table[bank]];

    pio_sm_set_enabled(SPI_PIO, t->sm, false);
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
    pio_sm_exec(SPI_PIO, t->sm, pio_encode_jmp(program_offset + SPI_REGMAP_ENTRY));
    t->wait_idle = idle_wait;
    t->window_open = active;
    t->command_taken = false;
    t->release_polls = 0;
    pio_sm_set_enabled(SPI_PIO, t->sm, !idle_wait);
}

static void __not_in_flash_func(stream_prepare)(struct metalclaw_spi_target *t, bool join) {
    struct metalclaw_stream *s = t->stream;
    const uint8_t *frame = s->frame[s->active];
    uint16_t length = s->length > METALCLAW_STREAM_FRAME_MAX ? METALCLAW_STREAM_FRAME_MAX : s->length;
    dma_channel_hw_t *tx = dma_channel_hw_addr((uint)t->tx_dma);
    dma_channel_hw_t *rx = dma_channel_hw_addr((uint)t->rx_dma);

    if (s->direction == METALCLAW_STREAM_OUT) {
        for (uint16_t i = 0; i < length; i++) {
            t->tx_buf[i] = wire_byte(t, frame[i]);
        }
    }

    pio_sm_set_enabled(SPI_PIO, t->sm, false);
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
            pio_encode_jmp(program_offset + (active ? SPI_STREAM_ACTIVE_ENTRY : SPI_STREAM_ENTRY)));
        t->window_open = active;
    }
    t->frame_pending = false;
    t->wait_idle = false;
    t->command_taken = false;
    t->release_polls = 0;
    pio_sm_set_enabled(SPI_PIO, t->sm, true);
}

static void __not_in_flash_func(prepare)(struct metalclaw_spi_target *t, bool join) {
    if (t->image == SPI_IMAGE_REGMAP) {
        regmap_prepare(t, join);
    } else if (t->image == SPI_IMAGE_STREAM) {
        stream_prepare(t, join);
    }
}

static bool __not_in_flash_func(command_pending)(const struct metalclaw_spi_target *t) {
    dma_channel_hw_t *rx = dma_channel_hw_addr((uint)t->rx_dma);

    return (rx->ctrl_trig & DMA_CH0_CTRL_TRIG_BUSY_BITS) != 0u || rx->transfer_count != 0u;
}

static void __not_in_flash_func(service_command)(struct metalclaw_spi_target *t) {
    dma_channel_hw_t *rx = dma_channel_hw_addr((uint)t->rx_dma);


    dma_irqn_acknowledge_channel(1u, (uint)t->rx_dma);
    dma_channel_set_irq1_enabled((uint)t->rx_dma, false);
    rx->write_addr = (uintptr_t)t->capture;
    rx->transfer_count = SPI_BUFFER_BYTES;
    rx->ctrl_trig = t->payload_ctrl;
    t->command_taken = true;
}

static bool __not_in_flash_func(command_byte)(const struct metalclaw_spi_target *t, uint8_t *out) {
    uintptr_t base = (uintptr_t)table_pool[t->table[t->bank]];
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

static void __not_in_flash_func(regmap_finalize)(struct metalclaw_spi_target *t) {
    struct metalclaw_regmap *map = t->map;
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
        for (uint32_t i = 0; i < served; i++) {
            uint8_t reg = t->framing.auto_increment ? (uint8_t)(address + i) : (uint8_t)address;

            (void)metalclaw_regmap_bus_read(map, reg);
        }
    } else {
        if (payload >= SPI_BUFFER_BYTES) {
            t->stats.long_windows++;
        }
        if (payload > SPI_BANK_BYTES) {
            payload = SPI_BANK_BYTES;
        }
        for (uint32_t i = 0; i < payload; i++) {
            uint8_t reg = t->framing.auto_increment ? (uint8_t)(address + i) : (uint8_t)address;

            metalclaw_regmap_bus_write(map, reg, t->capture[i]);
        }
    }
}

static void __not_in_flash_func(stream_finalize)(struct metalclaw_spi_target *t) {
    struct metalclaw_stream *s = t->stream;
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
    if (s->direction == METALCLAW_STREAM_IN && s->on_frame != NULL) {
        uint32_t len = count > SPI_FRAME_BYTES ? SPI_FRAME_BYTES : count;

        if (!t->msb_first) {
            for (uint32_t i = 0; i < len; i++) {
                t->capture[i] = reverse_byte(t->capture[i]);
            }
        }
        s->on_frame(s, t->capture, len, s->on_frame_ctx);
    }
}

static void __not_in_flash_func(finalize)(struct metalclaw_spi_target *t) {
    uint32_t state = metalclaw_hal_critical_enter();

    if (t->image == SPI_IMAGE_NONE || !t->window_open) {
        metalclaw_hal_critical_exit(state);
        return;
    }
    t->window_open = false;
    miso_release(t);
    if (t->image == SPI_IMAGE_REGMAP) {
        regmap_finalize(t);
    } else {
        stream_finalize(t);
    }
    prepare(t, true);
    metalclaw_hal_critical_exit(state);
}

static bool __not_in_flash_func(window_evidence)(const struct metalclaw_spi_target *t) {
    if (t->image == SPI_IMAGE_REGMAP) {
        return t->command_taken || dma_irqn_get_channel_status(1u, (uint)t->rx_dma);
    }
    if (t->image == SPI_IMAGE_STREAM) {
        return dma_channel_hw_addr((uint)t->rx_dma)->transfer_count != SPI_BUFFER_BYTES;
    }
    return false;
}

static void __not_in_flash_func(spi_target_dma_handler)(void) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct metalclaw_spi_target *t = &targets[i];

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
}

static void __not_in_flash_func(spi_target_release_handler)(void) {
    uint32_t flags = SPI_PIO->irq;

    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct metalclaw_spi_target *t = &targets[i];

        if (t->owner == NULL || (flags & (1u << t->monitor_sm)) == 0u) {
            continue;
        }
        pio_interrupt_clear(SPI_PIO, t->monitor_sm);
        if (t->image == SPI_IMAGE_NONE) {
            continue;
        }
        if (t->wait_idle) {
            t->wait_idle = false;
            pio_sm_exec(SPI_PIO, t->sm, pio_encode_jmp(program_offset + SPI_REGMAP_ENTRY));
            pio_sm_set_enabled(SPI_PIO, t->sm, true);
            continue;
        }
        if (!t->window_open && window_evidence(t)) {
            t->window_open = true;
        }
        if (t->window_open) {
            finalize(t);
        }
    }
}

static void stream_notify(struct metalclaw_stream *s, void *ctx) {
    struct metalclaw_spi_target *t = ctx;
    uint32_t state = metalclaw_hal_critical_enter();

    (void)s;
    if (t->image != SPI_IMAGE_STREAM) {
        metalclaw_hal_critical_exit(state);
        return;
    }
    if (t->window_open || !gpio_get(t->cs)) {
        t->frame_pending = true;
    } else {
        stream_prepare(t, false);
    }
    metalclaw_hal_critical_exit(state);
}

static void release_irq_sync(bool want) {
    if (want == release_installed) {
        return;
    }
    if (want) {
        irq_set_exclusive_handler(PIO1_IRQ_1, spi_target_release_handler);
        irq_set_priority(PIO1_IRQ_1, 0);
        irq_set_enabled(PIO1_IRQ_1, true);
    } else {
        irq_set_enabled(PIO1_IRQ_1, false);
        irq_remove_handler(PIO1_IRQ_1, spi_target_release_handler);
    }
    release_installed = want;
}

static void dma_handler_sync(bool want) {
    if (want == dma_installed) {
        return;
    }
    if (want) {
        irq_add_shared_handler(DMA_IRQ_1, spi_target_dma_handler,
            PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_priority(DMA_IRQ_1, 0);
        irq_set_enabled(DMA_IRQ_1, true);
    } else {
        irq_remove_handler(DMA_IRQ_1, spi_target_dma_handler);
    }
    dma_installed = want;
}

static void release_tables(struct metalclaw_spi_target *t) {
    uint8_t tag = (uint8_t)((t - targets) + 1);

    for (size_t i = 0; i < SPI_TABLE_SLOTS; i++) {
        if (table_owner[i] == tag) {
            table_owner[i] = 0;
        }
    }
    t->banks = 0;
}

static bool claim_tables(struct metalclaw_spi_target *t, uint8_t banks) {
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

static void build_tables(struct metalclaw_spi_target *t) {
    const struct metalclaw_regmap *map = t->map;

    for (uint8_t b = 0; b < t->banks; b++) {
        struct spi_descriptor *table = table_pool[t->table[b]];
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
static const uint8_t spi_bus_token;
#define SPI_BUS_OWNER ((const void *)&spi_bus_token)

static bool pin_used_elsewhere(const struct metalclaw_spi_target *t, uint8_t pin) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        const struct metalclaw_spi_target *o = &targets[i];

        if (o != t && o->owner != NULL && (o->sck == pin || o->mosi == pin || o->miso == pin)) {
            return true;
        }
    }
    return false;
}

static void release_bus_pin(const struct metalclaw_spi_target *t, uint8_t pin) {
    if (pin_used_elsewhere(t, pin)) {
        return;
    }
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);
    metalclaw_hal_pin_release(pin, SPI_BUS_OWNER);
}

static void release_pads(const struct metalclaw_spi_target *t) {
    hw_clear_bits(&SPI_PIO->input_sync_bypass, 1u << t->sck);
    release_bus_pin(t, t->sck);
    release_bus_pin(t, t->mosi);
    release_bus_pin(t, t->miso);
    gpio_set_function(t->cs, GPIO_FUNC_SIO);
    gpio_set_dir(t->cs, GPIO_IN);
    gpio_disable_pulls(t->cs);
    metalclaw_hal_pin_release(t->cs, t->owner);
}

struct metalclaw_spi_target *metalclaw_hal_spi_target_open(uint8_t sck, uint8_t mosi, uint8_t miso,
    uint8_t cs, uint8_t mode, bool msb_first, const void *owner, int *err) {
    struct metalclaw_spi_target *t = NULL;
    uint8_t claimed[4];
    uint8_t claims = 0;
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
    if (open_count != 0 && (sck != program_sck || cls != program_class)) {
        *err = -ENOSPC;
        return NULL;
    }

    claimed[0] = sck;
    claimed[1] = mosi;
    claimed[2] = miso;
    claimed[3] = cs;
    for (claims = 0; claims < 4; claims++) {
        *err = metalclaw_hal_pin_claim(claimed[claims], claims < 3u ? SPI_BUS_OWNER : owner);
        if (*err != 0) {
            while (claims > 0u) {
                claims--;
                if (claims == 3u) {
                    metalclaw_hal_pin_release(claimed[claims], owner);
                } else if (!pin_used_elsewhere(t, claimed[claims])) {
                    metalclaw_hal_pin_release(claimed[claims], SPI_BUS_OWNER);
                }
            }
            return NULL;
        }
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
    if (!program_loaded) {
        program_dummy = (uint16_t)pio_encode_nop();
        encode_program(sck, cls);
        if (!pio_can_add_program(SPI_PIO, &spi_program)) {
            *err = -ENOSPC;
            goto fail_dma;
        }
        program_offset = (uint8_t)pio_add_program(SPI_PIO, &spi_program);
        program_sck = sck;
        program_class = cls;
        program_loaded = true;
    }

    t->sck = sck;
    t->mosi = mosi;
    t->miso = miso;
    t->cs = cs;
    t->mode = mode;
    t->msb_first = msb_first;
    t->image = SPI_IMAGE_NONE;
    t->map = NULL;
    t->stream = NULL;
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
        sm_config_set_wrap(&c, program_offset + SPI_MONITOR_ENTRY, program_offset + SPI_MONITOR_WRAP_TOP);
        pio_sm_init(SPI_PIO, t->monitor_sm, program_offset + SPI_MONITOR_ENTRY, &c);
        pio_interrupt_clear(SPI_PIO, t->monitor_sm);
        pio_set_irq1_source_enabled(SPI_PIO, (enum pio_interrupt_source)(pis_interrupt0 + t->monitor_sm), true);
        pio_sm_set_enabled(SPI_PIO, t->monitor_sm, true);
    }

    t->owner = owner;
    open_count++;
    release_irq_sync(true);
    dma_handler_sync(true);
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
    for (size_t i = 0; i < 3u; i++) {
        if (!pin_used_elsewhere(t, claimed[i])) {
            metalclaw_hal_pin_release(claimed[i], SPI_BUS_OWNER);
        }
    }
    metalclaw_hal_pin_release(cs, owner);
    return NULL;
}

void metalclaw_hal_spi_target_disarm(struct metalclaw_spi_target *t) {
    uint32_t state;

    if (t == NULL || t->owner == NULL) {
        return;
    }
    state = metalclaw_hal_critical_enter();
    if (t->image != SPI_IMAGE_NONE) {
        pio_sm_set_enabled(SPI_PIO, t->sm, false);
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
        release_tables(t);
        t->image = SPI_IMAGE_NONE;
        t->map = NULL;
        t->stream = NULL;
    }
    metalclaw_hal_critical_exit(state);
}

void metalclaw_hal_spi_target_close(struct metalclaw_spi_target *t) {
    if (t == NULL || t->owner == NULL) {
        return;
    }
    metalclaw_hal_spi_target_disarm(t);

    pio_sm_set_enabled(SPI_PIO, t->sm, false);
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
    release_pads(t);
    t->owner = NULL;
    open_count--;
    if (open_count == 0u) {
        release_irq_sync(false);
        dma_handler_sync(false);
        if (program_loaded) {
            pio_remove_program(SPI_PIO, &spi_program, program_offset);
            program_loaded = false;
        }
    }
}

int metalclaw_hal_spi_target_set_mode(struct metalclaw_spi_target *t, uint8_t mode) {
    uint8_t cls;
    uint32_t state;

    if (t == NULL || t->owner == NULL || mode > 3) {
        return -EINVAL;
    }
    cls = mode_class(mode);
    if (cls != program_class) {
        for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
            const struct metalclaw_spi_target *o = &targets[i];

            if (o != t && o->owner != NULL && mode_class(o->mode) != cls) {
                return -ENOSPC;
            }
        }
    }
    state = metalclaw_hal_critical_enter();
    t->mode = mode;
    if (cls != program_class) {
        for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
            struct metalclaw_spi_target *o = &targets[i];

            if (o->owner != NULL) {
                pio_sm_set_enabled(SPI_PIO, o->sm, false);
                stop_dma(o);
                o->window_open = false;
            }
        }
        program_patch(program_sck, cls);
        for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
            struct metalclaw_spi_target *o = &targets[i];

            if (o->owner != NULL && o->image != SPI_IMAGE_NONE) {
                prepare(o, false);
            }
        }
    } else if (t->image != SPI_IMAGE_NONE) {
        pio_sm_set_enabled(SPI_PIO, t->sm, false);
        stop_dma(t);
        t->window_open = false;
        prepare(t, false);
    }
    metalclaw_hal_critical_exit(state);
    return 0;
}

int metalclaw_hal_spi_target_arm_regmap(struct metalclaw_spi_target *t, struct metalclaw_regmap *map,
    const struct metalclaw_regmap_framing *framing) {
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
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        const struct metalclaw_spi_target *o = &targets[i];

        if (o != t && o->owner != NULL && o->image == SPI_IMAGE_REGMAP &&
            dummy_instruction(&o->framing, o->map->filler) != dummy) {
            return -ENOSPC;
        }
    }
    metalclaw_hal_spi_target_disarm(t);
    banks = map->bank_count == 0 ? 1 : map->bank_count;
    if (banks > METALCLAW_REGMAP_BANKS_MAX) {
        banks = METALCLAW_REGMAP_BANKS_MAX;
    }
    state = metalclaw_hal_critical_enter();
    if (!claim_tables(t, banks)) {
        metalclaw_hal_critical_exit(state);
        return -ENOMEM;
    }
    t->map = map;
    t->framing = *framing;
    t->image = SPI_IMAGE_REGMAP;
    memset(t->tx_buf, map->filler, sizeof(t->tx_buf));
    build_tables(t);
    memset(&t->stats, 0, sizeof(t->stats));
    if (dummy != program_dummy) {
        program_patch_dummy(dummy);
    }
    configure_sm(t, true);
    pio_interrupt_clear(SPI_PIO, t->monitor_sm);
    regmap_prepare(t, false);
    metalclaw_hal_critical_exit(state);
    return 0;
}

int metalclaw_hal_spi_target_arm_stream(struct metalclaw_spi_target *t,
    struct metalclaw_stream *s) {
    uint32_t state;

    if (t == NULL || t->owner == NULL || s == NULL) {
        return -EINVAL;
    }
    if (s->length > METALCLAW_STREAM_FRAME_MAX) {
        return -EINVAL;
    }
    metalclaw_hal_spi_target_disarm(t);
    state = metalclaw_hal_critical_enter();
    t->stream = s;
    t->image = SPI_IMAGE_STREAM;
    t->frame_pending = false;
    memset(&t->stats, 0, sizeof(t->stats));
    s->notify = stream_notify;
    s->notify_ctx = t;
    configure_sm(t, false);
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
    metalclaw_hal_critical_exit(state);
    return 0;
}

void metalclaw_hal_spi_target_poll(void) {
    for (size_t i = 0; i < SPI_TARGET_COUNT; i++) {
        struct metalclaw_spi_target *t = &targets[i];

        if (t->owner == NULL || t->image == SPI_IMAGE_NONE) {
            continue;
        }
        if (t->wait_idle) {
            if (gpio_get(t->cs)) {
                uint32_t state = metalclaw_hal_critical_enter();

                if (t->wait_idle && gpio_get(t->cs)) {
                    t->wait_idle = false;
                    pio_sm_exec(SPI_PIO, t->sm,
                        pio_encode_jmp(program_offset + SPI_REGMAP_ENTRY));
                    pio_sm_set_enabled(SPI_PIO, t->sm, true);
                }
                metalclaw_hal_critical_exit(state);
            }
            continue;
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

void metalclaw_hal_spi_target_stats(const struct metalclaw_spi_target *t,
    struct metalclaw_spi_target_stats *out) {
    uint32_t state;

    if (out == NULL) {
        return;
    }
    if (t == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    state = metalclaw_hal_critical_enter();
    *out = t->stats;
    metalclaw_hal_critical_exit(state);
}

uint32_t metalclaw_hal_spi_target_max_clock_hz(const struct metalclaw_spi_target *t) {
    if (t == NULL) {
        return 0u;
    }
    switch (t->image) {
        case SPI_IMAGE_REGMAP:
            return SPI_REGMAP_CLOCK_HZ;
        case SPI_IMAGE_STREAM:
            return SPI_STREAM_CLOCK_HZ;
        case SPI_IMAGE_NONE:
        default:
            return 0u;
    }
}
