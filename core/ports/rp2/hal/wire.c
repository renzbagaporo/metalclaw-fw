// SPDX-License-Identifier: Apache-2.0 OR MIT
// hal/wire.h on rp2. Each engine is a PIO program that open() encodes from
// the facts' times and the harness clock, on the first block in
// mclw_pio_try()'s order with room; state machines with identical programs
// share one copy. Input engines stream through a DMA channel into a 2 KiB
// block of a static pool that the model's poll reads; nothing runs per bit
// but the 1-Wire engine's slot handler, once a slot, on DMA_IRQ_1 on the
// worker core, beside the SPI target engine's handler.
//
// Edges are timed to two harness cycles or so: the input synchronizer's
// two cycles and a polling loop's pass (8 to 16 ns at 125 MHz).

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/wire.h"
#include "metalclaw/onewire.h"
#include "pio_window.h"
#include "wire_programs.h"

#define WIRE_SLOTS 12u
#define WIRE_PROGRAMS 2u
#define WIRE_LOADED_MAX 8u
#define WIRE_BLOCKS 4u
#define WIRE_BLOCK_WORDS 512u
#define WIRE_BLOCK_BYTES (WIRE_BLOCK_WORDS * 4u)
#define WIRE_RING_BITS 11u
#define WIRE_RING16_ENTRIES (WIRE_BLOCK_BYTES / 2u)
// Entries a busy ring channel has counted that a read leaves for later.
#define WIRE_IN_FLIGHT 2u
#define WIRE_END_WORD 0xffffffffu
#define WIRE_BAD_WORD 0xfffffffeu

// RP2350 re-triggers a channel whose count runs out by itself; an RP2040
// channel stops, and a read restarts it.
#if PICO_RP2040
#define WIRE_COUNT_RELOAD 0xffffffffu
#define WIRE_COUNT_MASK 0xffffffffu
#define WIRE_DMA_COUNT WIRE_COUNT_RELOAD
#else
#define WIRE_COUNT_RELOAD 0x0fffffffu
#define WIRE_COUNT_MASK 0x0fffffffu
#define WIRE_DMA_COUNT (dma_encode_transfer_count_with_self_trigger(WIRE_COUNT_RELOAD))
#endif

struct loaded {
    PIO pio;
    uint8_t offset;
    uint8_t length;
    uint8_t users;
    uint16_t words[WIRE_PROGRAM_WORDS];
};

struct mclw_wire {
    const void *owner;
    struct mclw_wire_facts facts;
    uint8_t pins[MCLW_WIRE_PINS_MAX];
    uint8_t count;
    PIO pio;
    uint8_t programs;
    int8_t sm[WIRE_PROGRAMS];
    int8_t loaded[WIRE_PROGRAMS];
    int8_t dma;
    int8_t block;
    // The state machines' clock.
    uint32_t hz;
    // Ring engines: entries the channel has written since the start, from
    // the drop in its count, and entries read.
    uint32_t last_count;
    uint32_t produced;
    uint32_t taken;
    bool capturing;
    // RESPONDER: the waveform's words, whether it is armed, and responses
    // played.
    uint32_t response_words;
    bool armed;
    uint32_t responses;
    // STEPS_OUT: states per word.
    uint8_t states_per_word;
    // EDGES_IN: the passes a level is counted for, and, once an entry was
    // dropped, how many of those in the FIFO came before it.
    uint32_t limit;
    bool dropped;
    uint32_t before_drop;
    struct mclw_onewire *onewire;
    volatile uint32_t sample;
    struct mclw_wire_stats stats;
};

static struct mclw_wire slots[WIRE_SLOTS];
static struct loaded loaded[WIRE_LOADED_MAX];
static uint32_t pool[WIRE_BLOCKS * WIRE_BLOCK_WORDS] __attribute__((aligned(WIRE_BLOCK_BYTES)));
static uint8_t pool_owner[WIRE_BLOCKS];
static bool dma_handler_installed;

static uint64_t pin_mask(const struct mclw_wire *w) {
    uint64_t mask = 0;

    for (uint8_t i = 0; i < w->count; i++) {
        mask |= MCLW_PIO_PIN(w->pins[i]);
    }
    return mask;
}

// The pins a block must reach for this wire: its own and, for LATCHED_IN,
// which samples its whole window, the PINS wires its owner opened before
// it. Pins 16-27 alone fit either window of an RP2350B block, and a block
// left at base 16 would leave a panel's data on GPIO0-15 unsampled.
static uint64_t window_mask(const struct mclw_wire *w) {
    uint64_t mask = pin_mask(w);

    if (w->facts.engine == MCLW_WIRE_LATCHED_IN) {
        for (size_t i = 0; i < WIRE_SLOTS; i++) {
            if (&slots[i] != w && slots[i].owner == w->owner && slots[i].facts.engine == MCLW_WIRE_PINS) {
                mask |= pin_mask(&slots[i]);
            }
        }
    }
    return mask;
}

static int load_program(PIO pio, const struct wire_program *p) {
    int free_entry = -1;

    for (size_t i = 0; i < WIRE_LOADED_MAX; i++) {
        struct loaded *l = &loaded[i];
        if (l->users == 0u) {
            if (free_entry < 0) {
                free_entry = (int)i;
            }
            continue;
        }
        if (l->pio == pio && l->length == p->length &&
            memcmp(l->words, p->words, p->length * sizeof(p->words[0])) == 0) {
            l->users++;
            return (int)i;
        }
    }
    pio_program_t program = { .instructions = p->words, .length = p->length, .origin = -1, .pio_version = 0 };
    if (free_entry < 0 || !pio_can_add_program(pio, &program)) {
        return -1;
    }
    struct loaded *l = &loaded[free_entry];
    l->pio = pio;
    l->offset = (uint8_t)pio_add_program(pio, &program);
    l->length = p->length;
    l->users = 1;
    memcpy(l->words, p->words, p->length * sizeof(p->words[0]));
    return free_entry;
}

static void unload_program(int index) {
    struct loaded *l = &loaded[index];

    if (--l->users == 0u) {
        pio_program_t program = { .instructions = l->words, .length = l->length, .origin = -1, .pio_version = 0 };
        pio_remove_program(l->pio, &program, l->offset);
    }
}

static void release_pio(struct mclw_wire *w) {
    for (uint8_t i = 0; i < w->programs; i++) {
        if (w->sm[i] >= 0) {
            pio_sm_set_enabled(w->pio, (uint)w->sm[i], false);
            pio_sm_unclaim(w->pio, (uint)w->sm[i]);
            w->sm[i] = -1;
        }
        if (w->loaded[i] >= 0) {
            unload_program(w->loaded[i]);
            w->loaded[i] = -1;
        }
    }
    w->programs = 0;
}

static int acquire_pio(struct mclw_wire *w, const struct wire_program *programs, uint8_t count) {
    int err = -ENOSPC;

    for (size_t n = 0; n < MCLW_PIO_TRIES; n++) {
        PIO pio = mclw_pio_try(n);
        if (pio == NULL || !mclw_pio_window(pio, window_mask(w))) {
            continue;
        }
        w->pio = pio;
        w->programs = count;
        bool ok = true;
        for (uint8_t i = 0; i < count && ok; i++) {
            int sm = pio_claim_unused_sm(pio, false);
            w->sm[i] = (int8_t)sm;
            w->loaded[i] = sm < 0 ? (int8_t)-1 : (int8_t)load_program(pio, &programs[i]);
            if (sm < 0) {
                err = -EBUSY;
            }
            ok = sm >= 0 && w->loaded[i] >= 0;
        }
        if (ok) {
            return 0;
        }
        release_pio(w);
    }
    return err;
}

static uint offset_of(const struct mclw_wire *w, uint8_t program) {
    return loaded[w->loaded[program]].offset;
}

static uint32_t *block_words(const struct mclw_wire *w) {
    return &pool[(size_t)w->block * WIRE_BLOCK_WORDS];
}

static int claim_block(struct mclw_wire *w) {
    for (size_t i = 0; i < WIRE_BLOCKS; i++) {
        if (pool_owner[i] == 0u) {
            pool_owner[i] = (uint8_t)((w - slots) + 1);
            w->block = (int8_t)i;
            return 0;
        }
    }
    return -ENOMEM;
}

static void release_block(struct mclw_wire *w) {
    if (w->block >= 0) {
        pool_owner[w->block] = 0;
        w->block = -1;
    }
}

static pio_sm_config base_config(const struct mclw_wire *w, uint8_t program, uint8_t wrap_target, uint8_t wrap,
    uint32_t want_hz) {
    pio_sm_config c = pio_get_default_sm_config();
    uint offset = offset_of(w, program);
    uint32_t sys = clock_get_hz(clk_sys);

    sm_config_set_wrap(&c, offset + wrap_target, offset + wrap);
    if (want_hz == 0u || want_hz >= sys) {
        sm_config_set_clkdiv_int_frac8(&c, 1, 0);
    } else {
        uint64_t div256 = ((uint64_t)sys * 256u + want_hz / 2u) / want_hz;
        sm_config_set_clkdiv_int_frac8(&c, (uint32_t)(div256 >> 8), (uint8_t)(div256 & 0xffu));
    }
    return c;
}

static uint32_t sm_hz(uint32_t want_hz) {
    uint32_t sys = clock_get_hz(clk_sys);

    if (want_hz == 0u || want_hz >= sys) {
        return sys;
    }
    uint64_t div256 = ((uint64_t)sys * 256u + want_hz / 2u) / want_hz;
    return (uint32_t)(((uint64_t)sys * 256u) / div256);
}

// A 32-bit value into a register of a stopped state machine, through its
// TX FIFO; OSR is left empty unless it is the target.
static void load_register(PIO pio, uint sm, enum pio_src_dest reg, uint32_t value, uint32_t side) {
    pio_sm_put(pio, sm, value);
    pio_sm_exec(pio, sm, pio_encode_pull(false, true) | side);
    if (reg != pio_osr) {
        pio_sm_exec(pio, sm, pio_encode_mov(reg, pio_osr) | side);
        pio_sm_exec(pio, sm, pio_encode_out(pio_null, 32) | side);
    }
}

static void input_pin(uint8_t pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_disable_pulls(pin);
}

static uint32_t channel_count(int8_t channel) {
    return dma_channel_hw_addr((uint)channel)->transfer_count & WIRE_COUNT_MASK;
}

static void start_ring(struct mclw_wire *w, bool wide) {
    dma_channel_config c = dma_channel_get_default_config((uint)w->dma);

    channel_config_set_transfer_data_size(&c, wide ? DMA_SIZE_32 : DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, WIRE_RING_BITS);
    channel_config_set_dreq(&c, pio_get_dreq(w->pio, (uint)w->sm[0], false));
    channel_config_set_high_priority(&c, true);
    dma_channel_configure((uint)w->dma, &c, block_words(w), &w->pio->rxf[w->sm[0]], WIRE_DMA_COUNT, true);
    w->last_count = channel_count(w->dma);
    w->produced = 0;
    w->taken = 0;
}

static uint32_t words_since(uint32_t last, uint32_t now) {
    return now <= last ? last - now : last + (WIRE_COUNT_RELOAD - now);
}

static void restart_exhausted(struct mclw_wire *w) {
#if PICO_RP2040
    if (!dma_channel_is_busy((uint)w->dma) && channel_count(w->dma) == 0u) {
        dma_channel_set_trans_count((uint)w->dma, WIRE_DMA_COUNT, true);
        w->last_count = WIRE_COUNT_RELOAD;
    }
#else
    (void)w;
#endif
}

// The ring channel is settled when its FIFO was empty and its count did not
// move across both looks; until then the last entries counted may not have
// landed yet and wait for a later read.
static size_t ring_read(struct mclw_wire *w, uint32_t *out, size_t max, bool wide) {
    uint32_t entries = wide ? WIRE_BLOCK_WORDS : WIRE_RING16_ENTRIES;
    uint32_t count = channel_count(w->dma);
    bool empty = pio_sm_is_rx_fifo_empty(w->pio, (uint)w->sm[0]);
    bool settled = empty && channel_count(w->dma) == count;
    const uint16_t *ring16 = (const uint16_t *)block_words(w);
    const uint32_t *ring32 = block_words(w);
    size_t n = 0;

    w->produced += words_since(w->last_count, count);
    w->last_count = count;
    restart_exhausted(w);
    if (w->produced - w->taken > entries) {
        w->stats.overruns += w->produced - w->taken - entries;
        w->taken = w->produced - entries;
    }
    uint32_t ready = w->produced;
    if (!settled) {
        uint32_t backlog = w->produced - w->taken;
        ready -= backlog < WIRE_IN_FLIGHT ? backlog : WIRE_IN_FLIGHT;
    }
    while (w->taken != ready && n < max) {
        uint32_t at = w->taken % entries;
        uint32_t v = wide ? ring32[at] : ring16[at];
        if (!wide) {
            if (v == (WIRE_END_WORD & 0xffffu)) {
                v = MCLW_WIRE_END;
                w->stats.frames++;
            } else if (v == (WIRE_BAD_WORD & 0xffffu)) {
                v = MCLW_WIRE_BAD;
                w->stats.errors++;
            }
        }
        out[n++] = v;
        w->taken++;
    }
    w->stats.words += (uint32_t)n;
    return n;
}

static int open_pulse_bits(struct mclw_wire *w) {
    struct wire_program p;
    uint32_t hz = clock_get_hz(clk_sys);
    int err;

    wire_encode_pulse_bits(&p, &w->facts, hz);
    if ((err = acquire_pio(w, &p, 1)) != 0 || (err = claim_block(w)) != 0) {
        return err;
    }
    input_pin(w->pins[0]);
    pio_sm_config c = base_config(w, 0, 0, (uint8_t)(p.length - 1u), 0);
    sm_config_set_in_pins(&c, w->pins[0]);
    sm_config_set_jmp_pin(&c, w->pins[0]);
    sm_config_set_in_shift(&c, false, true, 8);
    pio_sm_init(w->pio, (uint)w->sm[0], offset_of(w, 0), &c);
    load_register(w->pio, (uint)w->sm[0], pio_y, wire_to_cycles(w->facts.pulse_bits.reset_ns, hz) / 2u, 0);
    w->hz = hz;
    start_ring(w, false);
    pio_sm_set_enabled(w->pio, (uint)w->sm[0], true);
    return 0;
}

static int open_clocked_in(struct mclw_wire *w) {
    struct wire_program p;
    int err;

    wire_encode_clocked_in(&p, (uint8_t)((w->pins[0] - w->pins[1]) & 31u));
    if ((err = acquire_pio(w, &p, 1)) != 0 || (err = claim_block(w)) != 0) {
        return err;
    }
    input_pin(w->pins[0]);
    input_pin(w->pins[1]);
    pio_sm_config c = base_config(w, 0, 0, 2, 0);
    sm_config_set_in_pins(&c, w->pins[1]);
    sm_config_set_in_shift(&c, false, true, 32);
    pio_sm_init(w->pio, (uint)w->sm[0], offset_of(w, 0), &c);
    w->hz = clock_get_hz(clk_sys);
    start_ring(w, true);
    pio_sm_set_enabled(w->pio, (uint)w->sm[0], true);
    return 0;
}

static int open_latched_in(struct mclw_wire *w) {
    struct wire_program p[2];
    uint8_t clock = w->pins[0];
    uint8_t latch = w->pins[1];
    uint8_t enable = w->pins[2];
    int err;

    wire_encode_latched_data(&p[0], (uint8_t)((latch - clock) & 31u));
    wire_encode_latched_enable(&p[1], (uint8_t)((latch - enable) & 31u));
    if ((err = acquire_pio(w, p, 2)) != 0 || (err = claim_block(w)) != 0) {
        return err;
    }
    for (uint8_t i = 0; i < 3u; i++) {
        input_pin(w->pins[i]);
    }
    pio_sm_config c = base_config(w, 0, 0, (uint8_t)(p[0].length - 1u), 0);
    sm_config_set_in_pins(&c, clock);
    sm_config_set_jmp_pin(&c, latch);
    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_out_shift(&c, true, false, 32);
    pio_sm_init(w->pio, (uint)w->sm[0], offset_of(w, 0), &c);
    c = base_config(w, 1, 0, (uint8_t)(p[1].length - 1u), 0);
    sm_config_set_in_pins(&c, enable);
    sm_config_set_jmp_pin(&c, latch);
    sm_config_set_in_shift(&c, true, false, 32);
    sm_config_set_out_shift(&c, true, false, 32);
    pio_sm_init(w->pio, (uint)w->sm[1], offset_of(w, 1), &c);
    w->hz = clock_get_hz(clk_sys);
    w->capturing = false;
    return 0;
}

static int open_responder(struct mclw_wire *w) {
    struct wire_program p;
    uint32_t hz = clock_get_hz(clk_sys);
    int err;

    wire_encode_responder(&p);
    if ((err = acquire_pio(w, &p, 1)) != 0 || (err = claim_block(w)) != 0) {
        return err;
    }
    input_pin(w->pins[0]);
    pio_sm_config c = base_config(w, 0, 0, (uint8_t)(p.length - 1u), 0);
    sm_config_set_in_pins(&c, w->pins[0]);
    sm_config_set_jmp_pin(&c, w->pins[0]);
    sm_config_set_sideset(&c, 1, false, true);
    sm_config_set_sideset_pins(&c, w->pins[0]);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    pio_sm_set_pins_with_mask64(w->pio, (uint)w->sm[0], 0, MCLW_PIO_PIN(w->pins[0]));
    pio_sm_set_pindirs_with_mask64(w->pio, (uint)w->sm[0], 0, MCLW_PIO_PIN(w->pins[0]));
    pio_gpio_init(w->pio, w->pins[0]);
    gpio_disable_pulls(w->pins[0]);
    pio_sm_init(w->pio, (uint)w->sm[0], offset_of(w, 0), &c);
    load_register(w->pio, (uint)w->sm[0], pio_isr, wire_to_cycles(w->facts.responder.start_min_ns, hz) / 2u,
        pio_encode_sideset(1, 0));
    w->hz = hz;
    w->response_words = 0;
    w->armed = false;
    pio_sm_set_enabled(w->pio, (uint)w->sm[0], true);
    return 0;
}

static int open_steps_out(struct mclw_wire *w) {
    struct wire_program p;
    int err;

    for (uint8_t i = 1; i < w->count; i++) {
        if (w->pins[i] != w->pins[0] + i) {
            return -EINVAL;
        }
    }
    wire_encode_steps_out(&p, w->count);
    if ((err = acquire_pio(w, &p, 1)) != 0) {
        return err;
    }
    w->states_per_word = (uint8_t)(32u / w->count);
    uint64_t mask = pin_mask(w);
    pio_sm_config c = base_config(w, 0, 0, 2, 0);
    sm_config_set_out_pins(&c, w->pins[0], w->count);
    sm_config_set_out_shift(&c, true, true, (uint)w->states_per_word * w->count);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    pio_sm_set_pins_with_mask64(w->pio, (uint)w->sm[0], 0, mask);
    pio_sm_set_pindirs_with_mask64(w->pio, (uint)w->sm[0], mask, mask);
    for (uint8_t i = 0; i < w->count; i++) {
        pio_gpio_init(w->pio, w->pins[i]);
        gpio_disable_pulls(w->pins[i]);
    }
    pio_sm_init(w->pio, (uint)w->sm[0], offset_of(w, 0), &c);
    w->hz = clock_get_hz(clk_sys);
    load_register(w->pio, (uint)w->sm[0], pio_y, wire_to_cycles(1000000u, w->hz), 0);
    pio_sm_set_enabled(w->pio, (uint)w->sm[0], true);
    return 0;
}

static int open_envelope(struct mclw_wire *w) {
    struct wire_program p;
    uint8_t in = w->pins[0];
    uint8_t out = w->pins[1];
    int err;

    wire_encode_envelope(&p);
    if ((err = acquire_pio(w, &p, 1)) != 0) {
        return err;
    }
    input_pin(in);
    pio_sm_config c = base_config(w, 0, 0, (uint8_t)(p.length - 1u), WIRE_PROGRAM_1MHZ);
    sm_config_set_in_pins(&c, in);
    sm_config_set_jmp_pin(&c, in);
    sm_config_set_sideset(&c, 1, false, false);
    sm_config_set_sideset_pins(&c, out);
    sm_config_set_in_shift(&c, false, false, 32);
    pio_sm_set_pins_with_mask64(w->pio, (uint)w->sm[0], MCLW_PIO_PIN(out), MCLW_PIO_PIN(out));
    pio_sm_set_pindirs_with_mask64(w->pio, (uint)w->sm[0], MCLW_PIO_PIN(out), MCLW_PIO_PIN(out));
    pio_gpio_init(w->pio, out);
    gpio_disable_pulls(out);
    pio_sm_init(w->pio, (uint)w->sm[0], offset_of(w, 0), &c);
    w->hz = sm_hz(WIRE_PROGRAM_1MHZ);
    uint32_t gap = wire_to_cycles(w->facts.envelope.gap_ns, w->hz) / 3u;
    load_register(w->pio, (uint)w->sm[0], pio_osr, gap > 0u ? gap : 1u, pio_encode_sideset(1, 1));
    // The OSR now holds the gap; joining the RX FIFO leaves no TX FIFO.
    hw_set_bits(&w->pio->sm[w->sm[0]].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
    pio_sm_set_enabled(w->pio, (uint)w->sm[0], true);
    return 0;
}

// The pin is read as an input whatever drives it; the limit goes into Y
// through the TX FIFO, which joining the RX FIFO then gives up.
static int open_edges_in(struct mclw_wire *w) {
    struct wire_program p;
    uint8_t pin = w->pins[0];
    int err;

    wire_encode_edges_in(&p);
    if ((err = acquire_pio(w, &p, 1)) != 0) {
        return err;
    }
    input_pin(pin);
    pio_sm_config c = base_config(w, 0, 0, (uint8_t)(p.length - 1u), 0);
    sm_config_set_in_pins(&c, pin);
    sm_config_set_jmp_pin(&c, pin);
    sm_config_set_in_shift(&c, false, false, 32);
    pio_sm_init(w->pio, (uint)w->sm[0], offset_of(w, 0), &c);
    w->hz = clock_get_hz(clk_sys);
    w->limit = wire_edges_limit(w->hz);
    load_register(w->pio, (uint)w->sm[0], pio_y, w->limit, 0);
    hw_set_bits(&w->pio->sm[w->sm[0]].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
    w->pio->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + (uint)w->sm[0]);
    pio_sm_set_enabled(w->pio, (uint)w->sm[0], true);
    return 0;
}

static int open_onewire(struct mclw_wire *w) {
    struct wire_program p;
    int err;

    wire_encode_onewire(&p, &w->facts);
    if ((err = acquire_pio(w, &p, 1)) != 0) {
        return err;
    }
    uint8_t pin = w->pins[0];
    input_pin(pin);
    pio_sm_config c = base_config(w, 0, 0, (uint8_t)(p.length - 1u), WIRE_PROGRAM_1MHZ);
    sm_config_set_in_pins(&c, pin);
    sm_config_set_jmp_pin(&c, pin);
    sm_config_set_sideset(&c, 1, false, true);
    sm_config_set_sideset_pins(&c, pin);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_out_shift(&c, true, false, 32);
    pio_sm_set_pins_with_mask64(w->pio, (uint)w->sm[0], 0, MCLW_PIO_PIN(pin));
    pio_sm_set_pindirs_with_mask64(w->pio, (uint)w->sm[0], 0, MCLW_PIO_PIN(pin));
    pio_gpio_init(w->pio, pin);
    gpio_disable_pulls(pin);
    pio_sm_init(w->pio, (uint)w->sm[0], offset_of(w, 0), &c);
    w->hz = sm_hz(WIRE_PROGRAM_1MHZ);
    w->onewire = NULL;
    return 0;
}

static void release_pins(struct mclw_wire *w) {
    for (uint8_t i = 0; i < w->count; i++) {
        gpio_disable_pulls(w->pins[i]);
        gpio_set_function(w->pins[i], GPIO_FUNC_SIO);
        gpio_set_dir(w->pins[i], GPIO_IN);
        mclw_hal_pin_release(w->pins[i], w->owner);
    }
}

static void __not_in_flash_func(wire_dma_handler)(void) {
    for (size_t i = 0; i < WIRE_SLOTS; i++) {
        struct mclw_wire *w = &slots[i];

        if (w->owner == NULL || w->onewire == NULL || w->dma < 0 || !dma_channel_get_irq1_status((uint)w->dma)) {
            continue;
        }
        dma_channel_acknowledge_irq1((uint)w->dma);
        uint32_t word = w->sample;
        uint32_t state = mclw_hal_critical_enter();
        if (word == WIRE_END_WORD) {
            mclw_onewire_reset(w->onewire);
            w->stats.frames++;
        } else {
            bool next = mclw_onewire_slot(w->onewire, (word & 1u) != 0u);
            if (!pio_sm_is_tx_fifo_full(w->pio, (uint)w->sm[0])) {
                pio_sm_put(w->pio, (uint)w->sm[0], next ? 1u : 0u);
            }
            w->stats.words++;
        }
        mclw_hal_critical_exit(state);
        dma_channel_set_trans_count((uint)w->dma, 1, true);
    }
}

static void dma_handler_install(void *ctx) {
    bool want = *(const bool *)ctx;

    if (want) {
        irq_add_shared_handler(DMA_IRQ_1, wire_dma_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_priority(DMA_IRQ_1, 0);
        irq_set_enabled(DMA_IRQ_1, true);
    } else {
        irq_remove_handler(DMA_IRQ_1, wire_dma_handler);
    }
}

static void dma_handler_sync(void) {
    bool want = false;

    for (size_t i = 0; i < WIRE_SLOTS; i++) {
        want = want || (slots[i].owner != NULL && slots[i].onewire != NULL);
    }
    if (want != dma_handler_installed) {
        mclw_hal_work_call(dma_handler_install, &want);
        dma_handler_installed = want;
    }
}

struct mclw_wire *mclw_hal_wire_open(const uint8_t *pins, uint8_t count, const struct mclw_wire_facts *facts,
    const void *owner, int *err) {
    struct mclw_wire *w = NULL;
    uint8_t claimed;

    if (err == NULL) {
        return NULL;
    }
    if (pins == NULL || facts == NULL || owner == NULL || count == 0u || count > MCLW_WIRE_PINS_MAX ||
        facts->engine == MCLW_WIRE_NONE) {
        *err = -EINVAL;
        return NULL;
    }
    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = 0; j < i; j++) {
            if (pins[i] == pins[j]) {
                *err = -EINVAL;
                return NULL;
            }
        }
    }
    for (size_t i = 0; i < WIRE_SLOTS && w == NULL; i++) {
        if (slots[i].owner == NULL) {
            w = &slots[i];
        }
    }
    if (w == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    for (claimed = 0; claimed < count; claimed++) {
        *err = mclw_hal_pin_claim(pins[claimed], owner);
        if (*err != 0) {
            while (claimed > 0u) {
                mclw_hal_pin_release(pins[--claimed], owner);
            }
            return NULL;
        }
    }
    memset(w, 0, sizeof(*w));
    w->facts = *facts;
    memcpy(w->pins, pins, count);
    w->count = count;
    w->sm[0] = w->sm[1] = -1;
    w->loaded[0] = w->loaded[1] = -1;
    w->dma = -1;
    w->block = -1;
    w->owner = owner;
    switch (facts->engine) {
        case MCLW_WIRE_PINS:
            for (uint8_t i = 0; i < count; i++) {
                input_pin(pins[i]);
            }
            *err = 0;
            return w;
        case MCLW_WIRE_PULSE_BITS_IN:
        case MCLW_WIRE_CLOCKED_IN:
        case MCLW_WIRE_LATCHED_IN:
        case MCLW_WIRE_ONEWIRE:
        case MCLW_WIRE_RESPONDER:
            w->dma = (int8_t)dma_claim_unused_channel(false);
            if (w->dma < 0) {
                *err = -ENOMEM;
                goto fail;
            }
            break;
        default:
            break;
    }
    switch (facts->engine) {
        case MCLW_WIRE_PULSE_BITS_IN:
            *err = open_pulse_bits(w);
            break;
        case MCLW_WIRE_CLOCKED_IN:
            *err = open_clocked_in(w);
            break;
        case MCLW_WIRE_LATCHED_IN:
            *err = open_latched_in(w);
            break;
        case MCLW_WIRE_ONEWIRE:
            *err = open_onewire(w);
            break;
        case MCLW_WIRE_RESPONDER:
            *err = open_responder(w);
            break;
        case MCLW_WIRE_STEPS_OUT:
            *err = open_steps_out(w);
            break;
        case MCLW_WIRE_ENVELOPE:
            *err = open_envelope(w);
            break;
        case MCLW_WIRE_EDGES_IN:
            *err = open_edges_in(w);
            break;
        default:
            *err = -ENOTSUP;
            break;
    }
    if (*err == 0) {
        return w;
    }
fail:
    release_pio(w);
    release_block(w);
    if (w->dma >= 0) {
        dma_channel_unclaim((uint)w->dma);
    }
    release_pins(w);
    w->owner = NULL;
    return NULL;
}

void mclw_hal_wire_close(struct mclw_wire *w) {
    if (w == NULL || w->owner == NULL) {
        return;
    }
    for (uint8_t i = 0; i < w->programs; i++) {
        if (w->sm[i] >= 0) {
            pio_sm_set_enabled(w->pio, (uint)w->sm[i], false);
        }
    }
    if (w->dma >= 0) {
        dma_channel_set_irq1_enabled((uint)w->dma, false);
        dma_channel_abort((uint)w->dma);
        dma_channel_acknowledge_irq1((uint)w->dma);
    }
    uint32_t state = mclw_hal_critical_enter();
    w->onewire = NULL;
    mclw_hal_critical_exit(state);
    dma_handler_sync();
    for (uint8_t i = 0; i < w->programs; i++) {
        if (w->sm[i] >= 0) {
            pio_sm_clear_fifos(w->pio, (uint)w->sm[i]);
        }
    }
    release_pio(w);
    release_block(w);
    if (w->dma >= 0) {
        dma_channel_unclaim((uint)w->dma);
        w->dma = -1;
    }
    release_pins(w);
    w->owner = NULL;
}

void mclw_hal_wire_stats(const struct mclw_wire *w, struct mclw_wire_stats *out) {
    uint32_t state = mclw_hal_critical_enter();
    *out = w->stats;
    mclw_hal_critical_exit(state);
}

uint8_t mclw_hal_wire_pin(const struct mclw_wire *w, uint8_t index) {
    return index < w->count ? w->pins[index] : MCLW_PIN_NONE;
}

int mclw_hal_wire_level(const struct mclw_wire *w, uint8_t index) {
    return index < w->count ? (gpio_get(w->pins[index]) ? 1 : 0) : -1;
}

int mclw_hal_wire_sample_bit(const struct mclw_wire *w, uint8_t pin) {
    if (w->facts.engine != MCLW_WIRE_LATCHED_IN) {
        return -1;
    }
    int bit = (int)pin - (int)pio_get_gpio_base(w->pio);
    return bit >= 0 && bit < 32 ? bit : -1;
}

size_t mclw_hal_wire_capture_words(const struct mclw_wire *w) {
    return w->facts.engine == MCLW_WIRE_LATCHED_IN ? WIRE_BLOCK_WORDS + 1u : 0u;
}

int mclw_hal_wire_capture(struct mclw_wire *w) {
    if (w == NULL || w->owner == NULL || w->facts.engine != MCLW_WIRE_LATCHED_IN) {
        return -EINVAL;
    }
    uint32_t mask = (1u << w->sm[0]) | (1u << w->sm[1]);
    pio_set_sm_mask_enabled(w->pio, mask, false);
    dma_channel_abort((uint)w->dma);
    for (uint8_t i = 0; i < 2u; i++) {
        pio_sm_clear_fifos(w->pio, (uint)w->sm[i]);
        pio_sm_restart(w->pio, (uint)w->sm[i]);
        pio_sm_exec(w->pio, (uint)w->sm[i], pio_encode_jmp(offset_of(w, i)));
    }
    dma_channel_config c = dma_channel_get_default_config((uint)w->dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(w->pio, (uint)w->sm[0], false));
    channel_config_set_high_priority(&c, true);
    dma_channel_configure((uint)w->dma, &c, block_words(w), &w->pio->rxf[w->sm[0]], WIRE_BLOCK_WORDS, true);
    w->capturing = true;
    pio_enable_sm_mask_in_sync(w->pio, mask);
    return 0;
}

// The data state machine pushed its latch word long before the enable
// one pushed its count (a whole latch period apart), so the channel has
// moved every sample once the count is in. A capture that overflowed the
// block before its latch is dropped, and the engine looks again.
static size_t latched_read(struct mclw_wire *w, uint32_t *out, size_t max) {
    if (!w->capturing || pio_sm_is_rx_fifo_empty(w->pio, (uint)w->sm[1])) {
        return 0;
    }
    uint32_t enabled = pio_sm_get(w->pio, (uint)w->sm[1]);
    uint32_t moved = WIRE_BLOCK_WORDS - channel_count(w->dma);
    const uint32_t *samples = block_words(w);
    uint32_t rotate = (uint32_t)(w->pins[0] - pio_get_gpio_base(w->pio)) & 31u;

    dma_channel_abort((uint)w->dma);
    w->capturing = false;
    if (moved == 0u || moved + 1u > max || (samples[moved - 1u] & 1u) != 0u) {
        w->stats.errors++;
        (void)mclw_hal_wire_capture(w);
        return 0;
    }
    for (uint32_t i = 0; i < moved; i++) {
        uint32_t v = samples[i];
        out[i] = rotate == 0u ? v : (v << rotate) | (v >> (32u - rotate));
    }
    out[moved] = wire_latched_enabled_ns(enabled, w->hz);
    w->stats.words += moved + 1u;
    w->stats.frames++;
    return moved + 1u;
}

static size_t envelope_read(struct mclw_wire *w, uint32_t *out, size_t max) {
    size_t n = 0;

    while (n < max && !pio_sm_is_rx_fifo_empty(w->pio, (uint)w->sm[0])) {
        uint32_t v = wire_envelope_entry(pio_sm_get(w->pio, (uint)w->sm[0]), w->hz);
        if ((v & MCLW_WIRE_MARK) != 0u) {
            w->stats.frames++;
        }
        out[n++] = v;
    }
    w->stats.words += (uint32_t)n;
    return n;
}

// A dropped entry shows as RXSTALL. The entries in the FIFO then came
// before it: they are handed out first, and the overrun counted once they
// are, so a reader that checks stats() between reads sees the loss where
// it was in the stream.
static size_t edges_read(struct mclw_wire *w, uint32_t *out, size_t max) {
    uint sm = (uint)w->sm[0];
    uint32_t stall = 1u << (PIO_FDEBUG_RXSTALL_LSB + sm);
    size_t n = 0;

    if (!w->dropped && (w->pio->fdebug & stall) != 0u) {
        w->pio->fdebug = stall;
        w->dropped = true;
        w->before_drop = pio_sm_get_rx_fifo_level(w->pio, sm);
    }
    size_t limit = w->dropped && w->before_drop < max ? w->before_drop : max;
    while (n < limit && !pio_sm_is_rx_fifo_empty(w->pio, sm)) {
        out[n++] = wire_edges_entry(pio_sm_get(w->pio, sm), w->hz, w->limit);
    }
    if (w->dropped) {
        w->before_drop -= (uint32_t)n;
        if (w->before_drop == 0u) {
            w->dropped = false;
            w->stats.overruns++;
        }
    }
    w->stats.words += (uint32_t)n;
    w->stats.frames += (uint32_t)n;
    return n;
}

size_t mclw_hal_wire_read(struct mclw_wire *w, uint32_t *out, size_t max) {
    if (w == NULL || w->owner == NULL || out == NULL) {
        return 0;
    }
    switch (w->facts.engine) {
        case MCLW_WIRE_PULSE_BITS_IN:
            return ring_read(w, out, max, false);
        case MCLW_WIRE_CLOCKED_IN:
            return ring_read(w, out, max, true);
        case MCLW_WIRE_LATCHED_IN:
            return latched_read(w, out, max);
        case MCLW_WIRE_ENVELOPE:
            return envelope_read(w, out, max);
        case MCLW_WIRE_EDGES_IN:
            return edges_read(w, out, max);
        default:
            return 0;
    }
}

static void responder_arm(struct mclw_wire *w) {
    dma_channel_config c = dma_channel_get_default_config((uint)w->dma);

    dma_channel_abort((uint)w->dma);
    pio_sm_clear_fifos(w->pio, (uint)w->sm[0]);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(w->pio, (uint)w->sm[0], true));
    dma_channel_configure((uint)w->dma, &c, &w->pio->txf[w->sm[0]], block_words(w), w->response_words, true);
    w->armed = true;
}

static bool responder_idle(const struct mclw_wire *w) {
    uint pc = pio_sm_get_pc(w->pio, (uint)w->sm[0]) - offset_of(w, 0);
    return pc <= RS_SAFE_END - 1u;
}

int mclw_hal_wire_respond(struct mclw_wire *w, const uint32_t *pulses, size_t count) {
    if (w == NULL || w->owner == NULL || w->facts.engine != MCLW_WIRE_RESPONDER || count + 1u > WIRE_BLOCK_WORDS) {
        return -EINVAL;
    }
    bool idle = !w->armed || responder_idle(w);
    uint32_t *words = block_words(w);
    if (!idle) {
        return -EBUSY;
    }
    dma_channel_abort((uint)w->dma);
    for (size_t i = 0; i < count; i++) {
        words[i] = wire_responder_word(pulses[i] & ~MCLW_WIRE_LOW, (pulses[i] & MCLW_WIRE_LOW) != 0u, w->hz);
    }
    words[count] = 0;
    w->response_words = (uint32_t)count + 1u;
    responder_arm(w);
    return 0;
}

uint32_t mclw_hal_wire_responses(struct mclw_wire *w) {
    if (w == NULL || w->owner == NULL || w->facts.engine != MCLW_WIRE_RESPONDER) {
        return 0;
    }
    if (w->armed && !dma_channel_is_busy((uint)w->dma) && pio_sm_is_tx_fifo_empty(w->pio, (uint)w->sm[0]) &&
        responder_idle(w)) {
        w->responses++;
        w->stats.frames++;
        responder_arm(w);
    }
    return w->responses;
}

int mclw_hal_wire_set_period(struct mclw_wire *w, uint32_t ns) {
    if (w == NULL || w->owner == NULL || w->facts.engine != MCLW_WIRE_STEPS_OUT) {
        return -EINVAL;
    }
    if (!mclw_hal_wire_idle(w)) {
        return -EBUSY;
    }
    uint32_t cycles = wire_to_cycles(ns, w->hz);
    uint sm = (uint)w->sm[0];
    pio_sm_set_enabled(w->pio, sm, false);
    load_register(w->pio, sm, pio_y, cycles > 4u ? cycles - 3u : 1u, 0);
    pio_sm_exec(w->pio, sm, pio_encode_jmp(offset_of(w, 0)));
    pio_sm_set_enabled(w->pio, sm, true);
    return 0;
}

// Idle once the TX FIFO is empty and the program stalls on it again after
// the stall flag is cleared: the last state has had its period.
bool mclw_hal_wire_idle(struct mclw_wire *w) {
    if (w == NULL || w->owner == NULL || w->facts.engine != MCLW_WIRE_STEPS_OUT) {
        return false;
    }
    uint32_t stall = 1u << (PIO_FDEBUG_TXSTALL_LSB + (uint)w->sm[0]);
    if (!pio_sm_is_tx_fifo_empty(w->pio, (uint)w->sm[0])) {
        return false;
    }
    w->pio->fdebug = stall;
    for (int i = 0; i < 4; i++) {
        if ((w->pio->fdebug & stall) != 0u) {
            return true;
        }
    }
    return false;
}

size_t mclw_hal_wire_write(struct mclw_wire *w, const uint32_t *words, size_t count) {
    size_t n = 0;

    if (w == NULL || w->owner == NULL || w->facts.engine != MCLW_WIRE_STEPS_OUT) {
        return 0;
    }
    while (n < count && !pio_sm_is_tx_fifo_full(w->pio, (uint)w->sm[0])) {
        pio_sm_put(w->pio, (uint)w->sm[0], words[n++]);
    }
    w->stats.words += (uint32_t)n;
    return n;
}

int mclw_hal_wire_onewire_arm(struct mclw_wire *w, struct mclw_onewire *image) {
    if (w == NULL || w->owner == NULL || w->facts.engine != MCLW_WIRE_ONEWIRE || image == NULL) {
        return -EINVAL;
    }
    uint sm = (uint)w->sm[0];
    pio_sm_set_enabled(w->pio, sm, false);
    dma_channel_abort((uint)w->dma);
    pio_sm_clear_fifos(w->pio, sm);
    pio_sm_restart(w->pio, sm);
    pio_sm_exec(w->pio, sm, pio_encode_jmp(offset_of(w, 0)) | pio_encode_sideset(1, 0));
    uint32_t state = mclw_hal_critical_enter();
    w->onewire = image;
    mclw_hal_critical_exit(state);
    dma_handler_sync();
    dma_channel_config c = dma_channel_get_default_config((uint)w->dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(w->pio, sm, false));
    channel_config_set_high_priority(&c, true);
    dma_channel_acknowledge_irq1((uint)w->dma);
    dma_channel_set_irq1_enabled((uint)w->dma, true);
    dma_channel_configure((uint)w->dma, &c, &w->sample, &w->pio->rxf[sm], 1, true);
    pio_sm_set_enabled(w->pio, sm, true);
    return 0;
}
