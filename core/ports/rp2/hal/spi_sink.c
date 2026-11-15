// SPDX-License-Identifier: Apache-2.0 OR MIT
// The receive-only SPI target. Two state machines on one PIO block, two DMA
// channels, and the worker's poll; no interrupt and no CPU per byte.
//
//   data   shifts each byte in, then one bit that is 0 for the first byte
//          of a window, then the side line's level, and autopushes the 10
//          bits
//   edge   pushes a JMP to data's restart at every chip-select rise; a DMA
//          channel writes it into data's instruction register, so data
//          drops any partial byte, marks the next byte first and waits for
//          chip select to fall again
//   ring   the other channel streams data's words into a ring the poll
//          reads; the poll also reports a window ended once chip select is
//          high with nothing left in flight
//
// The restart keeps data in step with the host whatever SCK does while
// chip select is high. Data finishes a window's last byte and goes on to
// wait for the next byte's first edge before the host raises chip select,
// so without the restart an SCK edge between windows would count as a bit
// and every byte after it would be off by one. On a bench whose lines have
// pull-ups the DUT's SCK makes such an edge whenever it resets: its pins
// float and the pull-up raises SCK, until the program drives it low again.
// The restart lands about ten cycles after the rise (the edge program's
// synchroniser and two instructions, then one DMA transfer); a host's
// first sampling edge comes at least half a clock after chip select falls,
// and chip select stays high for at least one clock between PL022 frames,
// so at 10 MHz on a 125 MHz harness about 60 ns are left over.
//
// Clock ceiling, from the programs rather than a bench measurement: after a
// sampling edge the bit loop needs IN, JMP and the next WAIT, and a byte's
// tail eight instructions before the first edge of the next, plus two
// cycles of input synchronizer each way: at 125 MHz that is about 15 MHz
// within a byte and 12.5 MHz with no gap between bytes. The published
// figure is 10 MHz, the SSD1306/SSD1309 and MAX7219 limit (SSD1306 Rev 1.1
// table 13-4 tcycle 100 ns; MAX7219 19-4452 Rev 4 timing tCP 100 ns).

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_sink.h"
#include "pio_window.h"

#define SINK_RING_ENTRIES 2048u
#define SINK_RING_BYTES (SINK_RING_ENTRIES * sizeof(uint16_t))
#define SINK_RING_BITS 12u
#define SINK_BATCH 64u
// Words a busy ring channel has counted that the poll leaves for later.
#define SINK_IN_FLIGHT 2u
#define SINK_CLOCK_HZ 10000000u

// RP2350 re-triggers a channel whose count runs out by itself; an RP2040
// channel stops, about an hour into continuous 10 MHz traffic, and the
// poll restarts it.
#if PICO_RP2040
#define SINK_COUNT_RELOAD 0xffffffffu
#define SINK_COUNT_MASK 0xffffffffu
#define SINK_DMA_COUNT SINK_COUNT_RELOAD
#else
#define SINK_COUNT_RELOAD 0x0fffffffu
#define SINK_COUNT_MASK 0x0fffffffu
#define SINK_DMA_COUNT (dma_encode_transfer_count_with_self_trigger(SINK_COUNT_RELOAD))
#endif

#define DATA_LENGTH 15u
#define DATA_RESTART 1u
#define DATA_WRAP_TARGET 4u
#define DATA_WRAP_TOP 14u
#define DATA_BIT 5u
#define DATA_LINE_HIGH 14u
#define EDGE_LENGTH 5u
#define EDGE_WRAP_TARGET 1u
#define WORD_BITS 10u

struct mclw_spi_sink {
    const void *owner;
    uint8_t sck;
    uint8_t mosi;
    uint8_t cs;
    uint8_t line;
    uint8_t mode;
    bool msb_first;
    PIO pio;
    uint8_t data_sm;
    uint8_t edge_sm;
    uint8_t data_offset;
    uint8_t edge_offset;
    int8_t ring_dma;
    int8_t token_dma;
    struct mclw_sink *sink;
    // Words the ring channel has written since the arm, from the drop in its
    // count; words delivered; whether the last delivery left a window open.
    uint32_t last_count;
    uint32_t produced;
    uint32_t taken;
    bool open;
    uint16_t batch[SINK_BATCH];
    struct mclw_spi_sink_stats stats;
};

static struct mclw_spi_sink slot;
static uint16_t ring[SINK_RING_ENTRIES] __attribute__((aligned(SINK_RING_BYTES)));
static uint16_t data_words[DATA_LENGTH];
static uint16_t edge_words[EDGE_LENGTH];
static bool poll_registered;

static const pio_program_t data_program = {
    .instructions = data_words,
    .length = DATA_LENGTH,
    .origin = -1,
    .pio_version = 0,
};

static const pio_program_t edge_program = {
    .instructions = edge_words,
    .length = EDGE_LENGTH,
    .origin = -1,
    .pio_version = 0,
};

// Modes 0 and 3 sample on the rising edge, 1 and 2 on the falling one. A
// WAIT GPIO holds the pin's low five bits; pio_add_program rebases it.
static void encode_programs(uint8_t sck, uint8_t cs, uint8_t mode) {
    bool edge = mode == 0u || mode == 3u;
    uint16_t *p = data_words;

    sck = (uint8_t)(sck % 32u);
    cs = (uint8_t)(cs % 32u);
    // Both programs start once chip select is high, so a window open at the
    // arm is skipped by both.
    p[0] = (uint16_t)pio_encode_wait_gpio(true, cs);
    p[DATA_RESTART] = (uint16_t)pio_encode_mov(pio_isr, pio_null);
    p[2] = (uint16_t)pio_encode_set(pio_y, 0u);
    p[3] = (uint16_t)pio_encode_wait_gpio(false, cs);
    p[4] = (uint16_t)pio_encode_set(pio_x, 7u);
    p[5] = (uint16_t)pio_encode_wait_gpio(!edge, sck);
    p[6] = (uint16_t)pio_encode_wait_gpio(edge, sck);
    p[7] = (uint16_t)pio_encode_in(pio_pins, 1u);
    p[8] = (uint16_t)pio_encode_jmp_x_dec(DATA_BIT);
    p[9] = (uint16_t)pio_encode_in(pio_y, 1u);
    p[10] = (uint16_t)pio_encode_set(pio_y, 1u);
    p[11] = (uint16_t)pio_encode_jmp_pin(DATA_LINE_HIGH);
    p[12] = (uint16_t)pio_encode_in(pio_null, 1u);
    p[13] = (uint16_t)pio_encode_jmp(DATA_WRAP_TARGET);
    // X is all ones once the bit loop has run out.
    p[14] = (uint16_t)pio_encode_in(pio_x, 1u);

    // X holds data's restart JMP, set at the arm.
    edge_words[0] = (uint16_t)pio_encode_wait_gpio(true, cs);
    edge_words[1] = (uint16_t)pio_encode_wait_gpio(false, cs);
    edge_words[2] = (uint16_t)pio_encode_wait_gpio(true, cs);
    edge_words[3] = (uint16_t)pio_encode_mov(pio_isr, pio_x);
    edge_words[4] = (uint16_t)pio_encode_push(false, false);
}

static uint8_t reverse_byte(uint8_t value) {
    uint32_t v = value;

    v = ((v & 0x55u) << 1) | ((v >> 1) & 0x55u);
    v = ((v & 0x33u) << 2) | ((v >> 2) & 0x33u);
    v = ((v & 0x0fu) << 4) | ((v >> 4) & 0x0fu);
    return (uint8_t)v;
}

static uint32_t channel_count(int8_t channel) {
    return dma_channel_hw_addr((uint)channel)->transfer_count & SINK_COUNT_MASK;
}

static int acquire_pio(struct mclw_spi_sink *s) {
    uint64_t pins = MCLW_PIO_PIN(s->sck) | MCLW_PIO_PIN(s->mosi) | MCLW_PIO_PIN(s->cs) |
        (s->line != MCLW_PIN_NONE ? MCLW_PIO_PIN(s->line) : 0u);
    int err = -ENOSPC;

    for (size_t n = 0; n < MCLW_PIO_TRIES; n++) {
        PIO pio = mclw_pio_try(n);
        if (pio == NULL || !pio_can_add_program(pio, &data_program) || !mclw_pio_window(pio, pins)) {
            continue;
        }
        int data_sm = pio_claim_unused_sm(pio, false);
        if (data_sm < 0) {
            err = -EBUSY;
            continue;
        }
        int edge_sm = pio_claim_unused_sm(pio, false);
        if (edge_sm < 0) {
            pio_sm_unclaim(pio, (uint)data_sm);
            err = -EBUSY;
            continue;
        }
        uint data_offset = pio_add_program(pio, &data_program);
        if (!pio_can_add_program(pio, &edge_program)) {
            pio_remove_program(pio, &data_program, data_offset);
            pio_sm_unclaim(pio, (uint)data_sm);
            pio_sm_unclaim(pio, (uint)edge_sm);
            continue;
        }
        s->pio = pio;
        s->data_sm = (uint8_t)data_sm;
        s->edge_sm = (uint8_t)edge_sm;
        s->data_offset = (uint8_t)data_offset;
        s->edge_offset = (uint8_t)pio_add_program(pio, &edge_program);
        return 0;
    }
    return err;
}

static void release_pio(struct mclw_spi_sink *s) {
    pio_set_sm_mask_enabled(s->pio, (1u << s->data_sm) | (1u << s->edge_sm), false);
    pio_remove_program(s->pio, &data_program, s->data_offset);
    pio_remove_program(s->pio, &edge_program, s->edge_offset);
    pio_sm_unclaim(s->pio, s->data_sm);
    pio_sm_unclaim(s->pio, s->edge_sm);
}

static void configure_sms(struct mclw_spi_sink *s) {
    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_in_pins(&c, s->mosi);
    sm_config_set_jmp_pin(&c, s->line != MCLW_PIN_NONE ? s->line : s->cs);
    sm_config_set_in_shift(&c, false, true, WORD_BITS);
    sm_config_set_out_shift(&c, false, false, 32u);
    sm_config_set_wrap(&c, s->data_offset + DATA_WRAP_TARGET, s->data_offset + DATA_WRAP_TOP);
    pio_sm_init(s->pio, s->data_sm, s->data_offset, &c);

    c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, s->edge_offset + EDGE_WRAP_TARGET, s->edge_offset + EDGE_LENGTH - 1u);
    pio_sm_init(s->pio, s->edge_sm, s->edge_offset, &c);
}

static void stop(struct mclw_spi_sink *s) {
    pio_set_sm_mask_enabled(s->pio, (1u << s->data_sm) | (1u << s->edge_sm), false);
    dma_channel_abort((uint)s->ring_dma);
    dma_channel_abort((uint)s->token_dma);
    pio_sm_clear_fifos(s->pio, s->data_sm);
    pio_sm_clear_fifos(s->pio, s->edge_sm);
}

static void start_dma(struct mclw_spi_sink *s) {
    dma_channel_config c = dma_channel_get_default_config((uint)s->ring_dma);

    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, SINK_RING_BITS);
    channel_config_set_dreq(&c, pio_get_dreq(s->pio, s->data_sm, false));
    channel_config_set_high_priority(&c, true);
    dma_channel_configure((uint)s->ring_dma, &c, ring, &s->pio->rxf[s->data_sm], SINK_DMA_COUNT, true);

    c = dma_channel_get_default_config((uint)s->token_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(s->pio, s->edge_sm, false));
    channel_config_set_high_priority(&c, true);
    dma_channel_configure((uint)s->token_dma, &c, &s->pio->sm[s->data_sm].instr, &s->pio->rxf[s->edge_sm],
        SINK_DMA_COUNT, true);
    s->last_count = channel_count(s->ring_dma);
}

// Words a channel moved between two looks at its count; across a self
// re-trigger the count went from `last` to 0 and on down from the reload.
static uint32_t words_since(uint32_t last, uint32_t now) {
    return now <= last ? last - now : last + (SINK_COUNT_RELOAD - now);
}

static bool restart_exhausted(int8_t channel) {
#if PICO_RP2040
    if (!dma_channel_is_busy((uint)channel) && channel_count(channel) == 0u) {
        dma_channel_set_transfer_count((uint)channel, SINK_DMA_COUNT, true);
        return true;
    }
#else
    (void)channel;
#endif
    return false;
}

static uint16_t entry_of(const struct mclw_spi_sink *s, uint16_t word) {
    uint8_t byte = (uint8_t)(word >> 2);
    uint16_t entry = s->msb_first ? byte : reverse_byte(byte);

    if ((word & 2u) == 0u) {
        entry |= MCLW_SINK_FIRST;
    }
    if (s->line != MCLW_PIN_NONE && (word & 1u) != 0u) {
        entry |= MCLW_SINK_LINE;
    }
    return entry;
}

// The worker's poll, under the runtime lock it only tries for. The ring
// channel is settled when its FIFO was empty and its count had not moved
// across both looks; until then the last words counted may not have landed
// in the ring yet, and wait for a later poll. A window is reported ended
// only when the channel was settled with chip select high: every word
// counted before then belongs to a window that is over.
static void deliver(struct mclw_spi_sink *s) {
    uint32_t count = channel_count(s->ring_dma);
    bool empty = pio_sm_is_rx_fifo_empty(s->pio, s->data_sm);
    bool high = gpio_get(s->cs);
    bool settled = empty && channel_count(s->ring_dma) == count;
    bool ended = settled && high;

    s->produced += words_since(s->last_count, count);
    s->last_count = count;
    if (restart_exhausted(s->ring_dma)) {
        s->last_count = SINK_COUNT_RELOAD;
    }
    (void)restart_exhausted(s->token_dma);
    if (s->produced - s->taken > SINK_RING_ENTRIES) {
        s->stats.overruns += s->produced - s->taken - SINK_RING_ENTRIES;
        s->taken = s->produced - SINK_RING_ENTRIES;
    }
    uint32_t ready = s->produced;
    if (!settled) {
        ready -= s->produced - s->taken < SINK_IN_FLIGHT ? s->produced - s->taken : SINK_IN_FLIGHT;
    }
    if (ready == s->taken) {
        if (ended && s->open) {
            s->open = false;
            s->sink->on_bytes(s->sink, s->batch, 0, true, s->sink->ctx);
        }
        return;
    }
    while (s->taken != ready) {
        uint32_t n = ready - s->taken;
        if (n > SINK_BATCH) {
            n = SINK_BATCH;
        }
        for (uint32_t i = 0; i < n; i++) {
            uint16_t entry = entry_of(s, ring[(s->taken + i) % SINK_RING_ENTRIES]);
            if ((entry & MCLW_SINK_FIRST) != 0u) {
                s->stats.windows++;
            }
            s->batch[i] = entry;
        }
        s->taken += n;
        s->stats.bytes += n;
        bool last = s->taken == ready && ready == s->produced;
        s->sink->on_bytes(s->sink, s->batch, n, last && ended, s->sink->ctx);
        s->open = !(last && ended);
    }
}

static void poll_sink(void *ctx) {
    (void)ctx;
    if (slot.owner == NULL || slot.sink == NULL || !mclw_hal_try_lock()) {
        return;
    }
    if (slot.sink != NULL) {
        deliver(&slot);
    }
    mclw_hal_unlock();
}

static void release_pins(struct mclw_spi_sink *s) {
    const uint8_t pins[4] = { s->sck, s->mosi, s->cs, s->line };

    for (size_t i = 0; i < 4u; i++) {
        if (pins[i] != MCLW_PIN_NONE) {
            gpio_disable_pulls(pins[i]);
            mclw_hal_pin_release(pins[i], s->owner);
        }
    }
}

struct mclw_spi_sink *mclw_hal_spi_sink_open(uint8_t sck, uint8_t mosi, uint8_t cs, uint8_t line,
    uint8_t mode, bool msb_first, const void *owner, int *err) {
    struct mclw_spi_sink *s = &slot;
    const uint8_t pins[4] = { sck, mosi, cs, line };
    size_t claimed;

    if (err == NULL) {
        return NULL;
    }
    if (owner == NULL || mode > 3u || sck == mosi || sck == cs || mosi == cs ||
        (line != MCLW_PIN_NONE && (line == sck || line == mosi || line == cs))) {
        *err = -EINVAL;
        return NULL;
    }
    if (s->owner != NULL) {
        *err = -EBUSY;
        return NULL;
    }
    if (!poll_registered) {
        if (mclw_hal_poll_register(poll_sink, NULL) != 0) {
            *err = -ENOSPC;
            return NULL;
        }
        poll_registered = true;
    }
    for (claimed = 0; claimed < 4u; claimed++) {
        if (pins[claimed] == MCLW_PIN_NONE) {
            continue;
        }
        *err = mclw_hal_pin_claim(pins[claimed], owner);
        if (*err != 0) {
            while (claimed > 0u) {
                claimed--;
                if (pins[claimed] != MCLW_PIN_NONE) {
                    mclw_hal_pin_release(pins[claimed], owner);
                }
            }
            return NULL;
        }
    }
    s->sck = sck;
    s->mosi = mosi;
    s->cs = cs;
    s->line = line;
    s->mode = mode;
    s->msb_first = msb_first;
    s->owner = owner;
    s->sink = NULL;
    encode_programs(sck, cs, mode);
    *err = acquire_pio(s);
    if (*err != 0) {
        goto fail_pins;
    }
    s->ring_dma = (int8_t)dma_claim_unused_channel(false);
    s->token_dma = (int8_t)dma_claim_unused_channel(false);
    if (s->ring_dma < 0 || s->token_dma < 0) {
        *err = -ENOMEM;
        goto fail_dma;
    }
    for (size_t i = 0; i < 4u; i++) {
        if (pins[i] != MCLW_PIN_NONE) {
            gpio_init(pins[i]);
            gpio_set_dir(pins[i], GPIO_IN);
            gpio_disable_pulls(pins[i]);
        }
    }
    // An undriven chip select reads deselected.
    gpio_pull_up(cs);
    s->stats = (struct mclw_spi_sink_stats){ 0 };
    *err = 0;
    return s;

fail_dma:
    if (s->token_dma >= 0) {
        dma_channel_unclaim((uint)s->token_dma);
    }
    if (s->ring_dma >= 0) {
        dma_channel_unclaim((uint)s->ring_dma);
    }
    release_pio(s);
fail_pins:
    release_pins(s);
    s->owner = NULL;
    return NULL;
}

void mclw_hal_spi_sink_close(struct mclw_spi_sink *s) {
    if (s == NULL || s->owner == NULL) {
        return;
    }
    mclw_hal_spi_sink_disarm(s);
    dma_channel_unclaim((uint)s->ring_dma);
    dma_channel_unclaim((uint)s->token_dma);
    release_pio(s);
    release_pins(s);
    s->owner = NULL;
}

int mclw_hal_spi_sink_arm(struct mclw_spi_sink *s, struct mclw_sink *sink) {
    if (s == NULL || s->owner == NULL || sink == NULL || sink->on_bytes == NULL) {
        return -EINVAL;
    }
    uint32_t state = mclw_hal_critical_enter();
    s->sink = NULL;
    mclw_hal_critical_exit(state);
    stop(s);
    configure_sms(s);
    start_dma(s);
    // A JMP to an address encodes as that address, which fits SET's 5 bits.
    pio_sm_exec(s->pio, s->edge_sm, pio_encode_set(pio_x, pio_encode_jmp(s->data_offset + DATA_RESTART)));
    s->produced = 0;
    s->taken = 0;
    s->open = false;
    state = mclw_hal_critical_enter();
    s->sink = sink;
    mclw_hal_critical_exit(state);
    pio_enable_sm_mask_in_sync(s->pio, (1u << s->data_sm) | (1u << s->edge_sm));
    return 0;
}

void mclw_hal_spi_sink_disarm(struct mclw_spi_sink *s) {
    if (s == NULL || s->owner == NULL) {
        return;
    }
    uint32_t state = mclw_hal_critical_enter();
    s->sink = NULL;
    mclw_hal_critical_exit(state);
    stop(s);
}

void mclw_hal_spi_sink_stats(const struct mclw_spi_sink *s, struct mclw_spi_sink_stats *out) {
    uint32_t state = mclw_hal_critical_enter();
    *out = s->stats;
    mclw_hal_critical_exit(state);
}

uint32_t mclw_hal_spi_sink_max_clock_hz(const struct mclw_spi_sink *s) {
    (void)s;
    return SINK_CLOCK_HZ;
}
