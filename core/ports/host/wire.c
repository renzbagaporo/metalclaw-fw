// SPDX-License-Identifier: Apache-2.0 OR MIT
// Wires for the host port: the rp2 port's PIO wire engines (hal/wire.h) in
// software, on the DUT's edges. Opening a wire asks the emulator to report
// every level the DUT drives on its pins (a watch action), each with its
// virtual time; the engine times them as the PIO program does, and drives
// the line with gpio actions at the times the part would.
//
//   PULSE_BITS_IN  a high pulse's width is a 0, a 1, or spoils the frame;
//                  a low of reset or longer ends the frame
//   CLOCKED_IN     data sampled at each rising clock edge, 32 bits a word
//   ONEWIRE        reset and presence, and each slot, with the core's
//                  1-Wire layer deciding the next
//   RESPONDER      a low of start_min or longer, then its release, plays
//                  the waveform
//   PINS           pins, no engine
//   STEPS_OUT      the states written, one a period, as timed gpio actions,
//                  up to STEPS_AHEAD words ahead of the DUT's time (the
//                  rp2 port's FIFO)
//   ENVELOPE       the output low from the carrier's first edge until no
//                  edge for gap_ns; each edge asks the emulator for a
//                  request at its gap's end, where the release goes out
// LATCHED_IN is not here yet.

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/wire.h"
#include "metalclaw/onewire.h"

#include "host.h"

#define WIRES_MAX 8u
#define RING 1024u
#define RESPONSE_MAX 128u
// A low this long or longer on a 1-Wire line is a reset (DS18B20 042208:
// tRSTL 480 us minimum; a slot is 60-120 us).
#define ONEWIRE_RESET_NS 300000u
#define STEPS_AHEAD 8u

struct mclw_wire {
    const void *owner;
    struct mclw_wire_facts facts;
    uint8_t pins[MCLW_WIRE_PINS_MAX];
    uint8_t count;
    struct mclw_wire_stats stats;
    // Captured entries for read().
    uint32_t ring[RING];
    uint32_t head;
    uint32_t tail;
    // The DUT's last level on each pin, and when it changed.
    int8_t level[MCLW_WIRE_PINS_MAX];
    uint64_t changed_ns[MCLW_WIRE_PINS_MAX];
    // PULSE_BITS_IN and CLOCKED_IN: the bits so far, and whether the
    // frame is open or spoiled.
    uint32_t shift;
    uint8_t bits;
    bool frame_open;
    bool spoiled;
    // ONEWIRE.
    struct mclw_onewire *onewire;
    bool hold_next;
    bool held;
    // RESPONDER.
    uint32_t pulses[RESPONSE_MAX];
    size_t pulse_count;
    uint32_t responses;
    // STEPS_OUT: the period, the last state put out, and when the queued
    // states run out.
    uint32_t period_ns;
    uint32_t last_state;
    uint64_t steps_end_ns;
    // ENVELOPE: whether a burst is on, when it began and ended, and the
    // carrier's last edge.
    bool burst;
    uint64_t burst_start_ns;
    uint64_t burst_end_ns;
    uint64_t last_edge_ns;
};

static struct mclw_wire wires[WIRES_MAX];
static bool poll_registered;

static void put(struct mclw_wire *w, uint32_t entry) {
    if (w->head - w->tail >= RING) {
        w->stats.overruns++;
        w->tail++;
    }
    w->ring[w->head++ % RING] = entry;
}

static void frame_end(struct mclw_wire *w) {
    if (w->frame_open) {
        put(w, MCLW_WIRE_END);
        w->stats.frames++;
    }
    w->frame_open = false;
    w->spoiled = false;
    w->bits = 0;
    w->shift = 0;
}

static void pulse_bits_edge(struct mclw_wire *w, bool level, uint64_t t_ns) {
    const uint64_t since = t_ns - w->changed_ns[0];

    if (level) {
        // The low before this rise: long enough, and the frame is over.
        if (w->level[0] == 0 && since >= w->facts.pulse_bits.reset_ns) {
            frame_end(w);
        }
        return;
    }
    if (w->level[0] != 1) {
        return;
    }
    // A high pulse ends: its width is the bit.
    w->frame_open = true;
    if (since >= w->facts.pulse_bits.zero_min_ns && since <= w->facts.pulse_bits.zero_max_ns) {
        w->shift <<= 1;
    } else if (since >= w->facts.pulse_bits.one_min_ns && since <= w->facts.pulse_bits.one_max_ns) {
        w->shift = (w->shift << 1) | 1u;
    } else {
        if (!w->spoiled) {
            put(w, MCLW_WIRE_BAD);
            w->stats.errors++;
        }
        w->spoiled = true;
        return;
    }
    if (++w->bits == 8u) {
        if (!w->spoiled) {
            put(w, w->shift & 0xffu);
        }
        w->bits = 0;
        w->shift = 0;
    }
}

static void clocked_edge(struct mclw_wire *w, uint8_t index, bool level) {
    if (index != 0u || !level || w->level[0] == 1) {
        return;
    }
    w->shift = (w->shift << 1) | (w->level[1] == 1 ? 1u : 0u);
    if (++w->bits == 32u) {
        put(w, w->shift);
        w->bits = 0;
        w->shift = 0;
    }
}

bool mclw_host_wire_spi_frame(uint8_t sck, uint8_t mosi_pin, uint8_t mosi) {
    bool taken = false;
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < WIRES_MAX; i++) {
        struct mclw_wire *w = &wires[i];

        if (w->owner == NULL || w->facts.engine != MCLW_WIRE_CLOCKED_IN || w->count < 2u ||
            w->pins[0] != sck || w->pins[1] != mosi_pin) {
            continue;
        }
        for (int b = 7; b >= 0; b--) {
            w->shift = (w->shift << 1) | ((mosi >> b) & 1u);
            if (++w->bits == 32u) {
                put(w, w->shift);
                w->bits = 0;
                w->shift = 0;
            }
        }
        taken = true;
    }
    mclw_hal_critical_exit(state);
    return taken;
}

static void onewire_edge(struct mclw_wire *w, bool level, uint64_t t_ns) {
    uint8_t pin = w->pins[0];
    uint32_t state;

    if (w->onewire == NULL) {
        return;
    }
    if (!level) {
        // A slot begins: the part holds the line through it if it is to
        // send a 0 (DS18B20 042208, figure 15: a 0 held for 15 us or more).
        if (w->hold_next) {
            mclw_host_event_gpio_at(pin, MCLW_HOST_GPIO_LOW, t_ns);
            mclw_host_event_gpio_at(pin, MCLW_HOST_GPIO_RELEASED, t_ns + w->facts.onewire.sample_ns);
        }
        w->held = w->hold_next;
        return;
    }
    uint64_t low = t_ns - w->changed_ns[0];

    state = mclw_hal_critical_enter();
    if (low >= ONEWIRE_RESET_NS) {
        mclw_onewire_reset(w->onewire);
        w->stats.frames++;
        w->hold_next = false;
        mclw_host_event_gpio_at(pin, MCLW_HOST_GPIO_LOW, t_ns + w->facts.onewire.presence_wait_ns);
        mclw_host_event_gpio_at(pin, MCLW_HOST_GPIO_RELEASED,
            t_ns + w->facts.onewire.presence_wait_ns + w->facts.onewire.presence_ns);
    } else {
        // The level the part sampled: its own 0, or the master's bit, a 1
        // if the master let go before the sample point.
        bool sampled = !w->held && low < w->facts.onewire.sample_ns;

        w->hold_next = !mclw_onewire_slot(w->onewire, sampled);
        w->stats.words++;
    }
    mclw_hal_critical_exit(state);
}

static void responder_edge(struct mclw_wire *w, bool level, uint64_t t_ns) {
    uint8_t pin = w->pins[0];
    uint64_t at = t_ns;

    if (!level || w->level[0] != 0 || t_ns - w->changed_ns[0] < w->facts.responder.start_min_ns ||
        w->pulse_count == 0u) {
        return;
    }
    for (size_t i = 0; i < w->pulse_count; i++) {
        uint32_t p = w->pulses[i];

        mclw_host_event_gpio_at(pin, (p & MCLW_WIRE_LOW) != 0u ? MCLW_HOST_GPIO_LOW : MCLW_HOST_GPIO_RELEASED, at);
        at += p & ~MCLW_WIRE_LOW;
    }
    mclw_host_event_gpio_at(pin, MCLW_HOST_GPIO_RELEASED, at);
    w->responses++;
    w->stats.frames++;
}

static void envelope_edge(struct mclw_wire *w, uint64_t t_ns) {
    if (!w->burst) {
        w->burst = true;
        w->burst_start_ns = t_ns;
        if (w->burst_end_ns != 0u) {
            put(w, (uint32_t)((t_ns - w->burst_end_ns) / 1000u) & ~MCLW_WIRE_MARK);
        }
        mclw_host_event_gpio_at(w->pins[1], MCLW_HOST_GPIO_LOW, t_ns);
    }
    w->last_edge_ns = t_ns;
    // A request when the gap would end, so the burst's end is seen then.
    mclw_host_event_wake(t_ns + w->facts.envelope.gap_ns);
}

void mclw_host_wire_edge(uint8_t pin, int level, uint64_t t_ns) {
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < WIRES_MAX; i++) {
        struct mclw_wire *w = &wires[i];

        if (w->owner == NULL) {
            continue;
        }
        for (uint8_t k = 0; k < w->count; k++) {
            if (w->pins[k] != pin) {
                continue;
            }
            // A released line reads high: every wire's line is pulled up.
            bool high = level != 0;

            if (w->level[k] == (high ? 1 : 0)) {
                continue;
            }
            switch (w->facts.engine) {
                case MCLW_WIRE_PULSE_BITS_IN:
                    pulse_bits_edge(w, high, t_ns);
                    break;
                case MCLW_WIRE_CLOCKED_IN:
                    clocked_edge(w, k, high);
                    break;
                case MCLW_WIRE_ONEWIRE:
                    onewire_edge(w, high, t_ns);
                    break;
                case MCLW_WIRE_RESPONDER:
                    responder_edge(w, high, t_ns);
                    break;
                case MCLW_WIRE_ENVELOPE:
                    if (k == 0u) {
                        envelope_edge(w, t_ns);
                    }
                    break;
                default:
                    break;
            }
            w->level[k] = high ? 1 : 0;
            w->changed_ns[k] = t_ns;
        }
    }
    mclw_hal_critical_exit(state);
}

// A line that stays low past the reset ends an LED frame without another
// edge; a carrier that stops ends its burst gap_ns after its last edge.
// Checked at every request, the wake a burst's edges ask for among them,
// so the envelope's release goes out at its time.
void mclw_host_wire_poll(uint64_t now) {
    uint32_t state = mclw_hal_critical_enter();
    for (size_t i = 0; i < WIRES_MAX; i++) {
        struct mclw_wire *w = &wires[i];

        if (w->owner != NULL && w->facts.engine == MCLW_WIRE_PULSE_BITS_IN && w->frame_open &&
            w->level[0] == 0 && now - w->changed_ns[0] >= w->facts.pulse_bits.reset_ns) {
            frame_end(w);
        }
        if (w->owner != NULL && w->facts.engine == MCLW_WIRE_ENVELOPE && w->burst &&
            now - w->last_edge_ns >= w->facts.envelope.gap_ns) {
            w->burst = false;
            w->burst_end_ns = w->last_edge_ns;
            put(w, MCLW_WIRE_MARK | ((uint32_t)((w->last_edge_ns - w->burst_start_ns) / 1000u) & ~MCLW_WIRE_MARK));
            w->stats.frames++;
            mclw_host_event_gpio_at(w->pins[1], MCLW_HOST_GPIO_RELEASED, w->last_edge_ns + w->facts.envelope.gap_ns);
        }
    }
    mclw_hal_critical_exit(state);
}

static void poll_wires(void *ctx) {
    (void)ctx;
    mclw_host_wire_poll(mclw_host_event_ns());
}

struct mclw_wire *mclw_hal_wire_open(const uint8_t *pins, uint8_t count, const struct mclw_wire_facts *facts,
    const void *owner, int *err) {
    struct mclw_wire *w = NULL;
    uint8_t claimed = 0;

    if (owner == NULL || facts == NULL || count == 0u || count > MCLW_WIRE_PINS_MAX) {
        *err = -EINVAL;
        return NULL;
    }
    switch (facts->engine) {
        case MCLW_WIRE_PULSE_BITS_IN:
        case MCLW_WIRE_CLOCKED_IN:
        case MCLW_WIRE_ONEWIRE:
        case MCLW_WIRE_RESPONDER:
        case MCLW_WIRE_PINS:
        case MCLW_WIRE_STEPS_OUT:
        case MCLW_WIRE_ENVELOPE:
            break;
        default:
            *err = -ENOTSUP;
            return NULL;
    }
    if (!poll_registered) {
        if (mclw_hal_poll_register(poll_wires, NULL) != 0) {
            *err = -ENOSPC;
            return NULL;
        }
        poll_registered = true;
    }
    uint32_t state = mclw_hal_critical_enter();
    for (size_t i = 0; i < WIRES_MAX && w == NULL; i++) {
        if (wires[i].owner == NULL) {
            w = &wires[i];
        }
    }
    mclw_hal_critical_exit(state);
    if (w == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    for (; claimed < count; claimed++) {
        *err = mclw_hal_pin_claim(pins[claimed], owner);
        if (*err != 0) {
            while (claimed-- > 0u) {
                mclw_hal_pin_release(pins[claimed], owner);
            }
            return NULL;
        }
    }
    state = mclw_hal_critical_enter();
    memset(w, 0, sizeof(*w));
    w->facts = *facts;
    w->count = count;
    memcpy(w->pins, pins, count);
    for (uint8_t k = 0; k < count; k++) {
        w->level[k] = -1;
    }
    w->owner = owner;
    mclw_hal_critical_exit(state);
    if (facts->engine == MCLW_WIRE_STEPS_OUT) {
        // The first state, every pin low, until states are written.
        for (uint8_t k = 0; k < count; k++) {
            mclw_host_event_gpio(pins[k], MCLW_HOST_GPIO_LOW);
        }
    } else if (facts->engine == MCLW_WIRE_ENVELOPE) {
        mclw_host_event_watch(pins[0]);
        mclw_host_event_gpio(pins[1], MCLW_HOST_GPIO_RELEASED);
    } else if (facts->engine != MCLW_WIRE_PINS) {
        for (uint8_t k = 0; k < count; k++) {
            mclw_host_event_watch(pins[k]);
        }
    }
    return w;
}

void mclw_hal_wire_close(struct mclw_wire *w) {
    if (w == NULL || w->owner == NULL) {
        return;
    }
    const void *owner = w->owner;
    uint32_t state = mclw_hal_critical_enter();

    w->owner = NULL;
    w->onewire = NULL;
    mclw_hal_critical_exit(state);
    for (uint8_t k = 0; k < w->count; k++) {
        mclw_hal_pin_release(w->pins[k], owner);
    }
}

void mclw_hal_wire_stats(const struct mclw_wire *w, struct mclw_wire_stats *out) {
    uint32_t state = mclw_hal_critical_enter();

    *out = w->stats;
    mclw_hal_critical_exit(state);
}

uint8_t mclw_hal_wire_pin(const struct mclw_wire *w, uint8_t index) {
    return index < w->count ? w->pins[index] : MCLW_PIN_NONE;
}

int mclw_hal_wire_sample_bit(const struct mclw_wire *w, uint8_t pin) {
    (void)w;
    (void)pin;
    return -1;
}

size_t mclw_hal_wire_read(struct mclw_wire *w, uint32_t *out, size_t max) {
    size_t n = 0;
    uint32_t state = mclw_hal_critical_enter();

    while (w->tail != w->head && n < max) {
        out[n++] = w->ring[w->tail++ % RING];
    }
    mclw_hal_critical_exit(state);
    return n;
}

int mclw_hal_wire_capture(struct mclw_wire *w) {
    (void)w;
    return -ENOTSUP;
}

size_t mclw_hal_wire_capture_words(const struct mclw_wire *w) {
    (void)w;
    return 0;
}

int mclw_hal_wire_respond(struct mclw_wire *w, const uint32_t *pulses, size_t count) {
    if (w->facts.engine != MCLW_WIRE_RESPONDER || count > RESPONSE_MAX) {
        return -EINVAL;
    }
    uint32_t state = mclw_hal_critical_enter();
    memcpy(w->pulses, pulses, count * sizeof(pulses[0]));
    w->pulse_count = count;
    mclw_hal_critical_exit(state);
    return 0;
}

uint32_t mclw_hal_wire_responses(struct mclw_wire *w) {
    return w->responses;
}

int mclw_hal_wire_set_period(struct mclw_wire *w, uint32_t ns) {
    if (w->facts.engine != MCLW_WIRE_STEPS_OUT || ns == 0u) {
        return -EINVAL;
    }
    if (!mclw_hal_wire_idle(w)) {
        return -EBUSY;
    }
    w->period_ns = ns;
    return 0;
}

// Each word holds 32 / pins states, the first in the low bits; a state's
// bit k is pin k.
size_t mclw_hal_wire_write(struct mclw_wire *w, const uint32_t *words, size_t count) {
    uint64_t now = mclw_host_event_ns();
    uint32_t per_word = 32u / w->count;
    uint32_t mask = (1u << w->count) - 1u;
    size_t n = 0;

    if (w->facts.engine != MCLW_WIRE_STEPS_OUT || w->period_ns == 0u) {
        return 0;
    }
    uint32_t state = mclw_hal_critical_enter();
    if (w->steps_end_ns < now) {
        w->steps_end_ns = now;
    }
    for (; n < count; n++) {
        if (w->steps_end_ns > now + (uint64_t)STEPS_AHEAD * per_word * w->period_ns) {
            break;
        }
        for (uint32_t s = 0; s < per_word; s++) {
            uint32_t next = (words[n] >> (s * w->count)) & mask;

            for (uint8_t k = 0; k < w->count; k++) {
                if (((next ^ w->last_state) >> k) & 1u) {
                    mclw_host_event_gpio_at(w->pins[k], ((next >> k) & 1u) ? MCLW_HOST_GPIO_HIGH
                                                                          : MCLW_HOST_GPIO_LOW,
                        w->steps_end_ns);
                }
            }
            w->last_state = next;
            w->steps_end_ns += w->period_ns;
        }
        w->stats.words++;
    }
    mclw_hal_critical_exit(state);
    return n;
}

bool mclw_hal_wire_idle(struct mclw_wire *w) {
    return w->facts.engine != MCLW_WIRE_STEPS_OUT || w->steps_end_ns <= mclw_host_event_ns();
}

int mclw_hal_wire_onewire_arm(struct mclw_wire *w, struct mclw_onewire *image) {
    if (w->facts.engine != MCLW_WIRE_ONEWIRE) {
        return -EINVAL;
    }
    uint32_t state = mclw_hal_critical_enter();
    w->onewire = image;
    w->hold_next = false;
    mclw_hal_critical_exit(state);
    return 0;
}
