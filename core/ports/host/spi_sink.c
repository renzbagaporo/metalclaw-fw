// SPDX-License-Identifier: Apache-2.0 OR MIT
// Receive-only SPI targets for the host port: each MOSI byte of a window
// on the sink's bus, marked as the window's first and with the side line's
// level (a data/command select the DUT drives, which the emulator reports
// once the sink watches it), handed to the model's on_bytes on the worker,
// as the rp2 port's PIO sink does (metalclaw/hal/spi_sink.h).

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_sink.h"

#include "host.h"

#define SINKS_MAX 4u
#define RING 4096u
// Bytes go to the model when a quarter of the ring is full and when a
// window ends: a burst of frames comes with no worker round between them.
#define DELIVER_AT (RING / 4u)

struct mclw_spi_sink {
    const void *owner;
    uint8_t sck;
    uint8_t mosi;
    uint8_t cs;
    uint8_t line;
    uint8_t mode;
    bool msb_first;
    struct mclw_sink *sink;
    bool selected;
    bool first;
    bool line_high;
    bool ended;
    bool open;
    uint32_t head;
    uint32_t tail;
    uint16_t ring[RING];
    struct mclw_spi_sink_stats stats;
};

static struct mclw_spi_sink sinks[SINKS_MAX];
static bool poll_registered;

static void deliver(struct mclw_spi_sink *s) {
    for (;;) {
        uint32_t state = mclw_hal_critical_enter();
        struct mclw_sink *sink = s->sink;
        uint32_t head = s->head;
        uint32_t tail = s->tail;
        bool ended = s->ended;

        mclw_hal_critical_exit(state);
        if (sink == NULL) {
            return;
        }
        uint32_t pending = head - tail;

        if (pending == 0u) {
            if (ended && s->open) {
                s->open = false;
                state = mclw_hal_critical_enter();
                s->ended = false;
                mclw_hal_critical_exit(state);
                sink->on_bytes(sink, s->ring, 0, true, sink->ctx);
            }
            return;
        }
        uint32_t at = tail % RING;
        uint32_t count = pending < RING - at ? pending : RING - at;
        bool last = count == pending;

        sink->on_bytes(sink, &s->ring[at], count, last && ended, sink->ctx);
        s->open = !(last && ended);
        state = mclw_hal_critical_enter();
        s->tail = tail + count;
        if (last && ended) {
            s->ended = false;
        }
        mclw_hal_critical_exit(state);
    }
}

static void poll_sinks(void *ctx) {
    (void)ctx;
    if (!mclw_hal_try_lock()) {
        return;
    }
    for (size_t i = 0; i < SINKS_MAX; i++) {
        if (sinks[i].owner != NULL && sinks[i].sink != NULL) {
            deliver(&sinks[i]);
        }
    }
    mclw_hal_unlock();
}

static struct mclw_work sink_work;

static void sink_work_fn(struct mclw_work *work) {
    (void)work;
    poll_sinks(NULL);
}

static void due(void) {
    sink_work.fn = sink_work_fn;
    mclw_hal_work_post(&sink_work);
}

bool mclw_host_spi_sink_cs(uint8_t pin, bool level) {
    bool any = false;
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < SINKS_MAX; i++) {
        struct mclw_spi_sink *s = &sinks[i];

        if (s->owner == NULL || s->cs != pin) {
            continue;
        }
        any = true;
        if (!level && !s->selected) {
            s->selected = true;
            s->first = true;
            s->stats.windows++;
        } else if (level && s->selected) {
            s->selected = false;
            s->ended = true;
        }
    }
    mclw_hal_critical_exit(state);
    if (any && level) {
        due();
    }
    return any;
}

void mclw_host_spi_sink_line(uint8_t pin, int level) {
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < SINKS_MAX; i++) {
        if (sinks[i].owner != NULL && sinks[i].line == pin) {
            sinks[i].line_high = level != 0;
        }
    }
    mclw_hal_critical_exit(state);
}

bool mclw_host_spi_sink_frame(uint8_t sck, uint8_t mosi_pin, uint8_t mosi) {
    bool taken = false;
    bool full = false;
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < SINKS_MAX; i++) {
        struct mclw_spi_sink *s = &sinks[i];

        if (s->owner == NULL || !s->selected || s->sck != sck || s->mosi != mosi_pin) {
            continue;
        }
        taken = true;
        if (s->sink == NULL) {
            continue;
        }
        if (s->head - s->tail >= RING) {
            s->stats.overruns++;
        } else {
            uint16_t entry = (uint16_t)(mosi | (s->first ? MCLW_SINK_FIRST : 0u) |
                (s->line != MCLW_PIN_NONE && s->line_high ? MCLW_SINK_LINE : 0u));

            s->ring[s->head++ % RING] = entry;
            s->stats.bytes++;
        }
        s->first = false;
        full = full || s->head - s->tail >= DELIVER_AT;
    }
    mclw_hal_critical_exit(state);
    if (full) {
        due();
    }
    return taken;
}

struct mclw_spi_sink *mclw_hal_spi_sink_open(uint8_t sck, uint8_t mosi, uint8_t cs, uint8_t line,
    uint8_t mode, bool msb_first, const void *owner, int *err) {
    struct mclw_spi_sink *s = NULL;

    if (owner == NULL || mode > 3u) {
        *err = -EINVAL;
        return NULL;
    }
    if (!poll_registered) {
        if (mclw_hal_poll_register(poll_sinks, NULL) != 0) {
            *err = -ENOSPC;
            return NULL;
        }
        poll_registered = true;
    }
    uint32_t state = mclw_hal_critical_enter();
    for (size_t i = 0; i < SINKS_MAX && s == NULL; i++) {
        if (sinks[i].owner == NULL) {
            s = &sinks[i];
        }
    }
    mclw_hal_critical_exit(state);
    if (s == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    *err = mclw_hal_pin_claim(cs, owner);
    if (*err == 0 && line != MCLW_PIN_NONE) {
        *err = mclw_hal_pin_claim(line, owner);
        if (*err != 0) {
            mclw_hal_pin_release(cs, owner);
        }
    }
    if (*err != 0) {
        return NULL;
    }
    state = mclw_hal_critical_enter();
    memset(s, 0, sizeof(*s));
    s->sck = sck;
    s->mosi = mosi;
    s->cs = cs;
    s->line = line;
    s->mode = mode;
    s->msb_first = msb_first;
    s->owner = owner;
    mclw_hal_critical_exit(state);
    mclw_host_event_watch(cs);
    if (line != MCLW_PIN_NONE) {
        mclw_host_event_watch(line);
    }
    return s;
}

void mclw_hal_spi_sink_close(struct mclw_spi_sink *s) {
    if (s == NULL || s->owner == NULL) {
        return;
    }
    const void *owner = s->owner;
    uint32_t state = mclw_hal_critical_enter();

    s->owner = NULL;
    s->sink = NULL;
    mclw_hal_critical_exit(state);
    mclw_hal_pin_release(s->cs, owner);
    if (s->line != MCLW_PIN_NONE) {
        mclw_hal_pin_release(s->line, owner);
    }
}

int mclw_hal_spi_sink_arm(struct mclw_spi_sink *s, struct mclw_sink *sink) {
    if (s == NULL || s->owner == NULL || sink == NULL || sink->on_bytes == NULL) {
        return -EINVAL;
    }
    uint32_t state = mclw_hal_critical_enter();
    s->head = 0;
    s->tail = 0;
    s->ended = false;
    s->open = false;
    s->sink = sink;
    mclw_hal_critical_exit(state);
    return 0;
}

void mclw_hal_spi_sink_disarm(struct mclw_spi_sink *s) {
    uint32_t state = mclw_hal_critical_enter();

    s->sink = NULL;
    mclw_hal_critical_exit(state);
}

void mclw_hal_spi_sink_stats(const struct mclw_spi_sink *s, struct mclw_spi_sink_stats *out) {
    uint32_t state = mclw_hal_critical_enter();

    *out = s->stats;
    mclw_hal_critical_exit(state);
}

uint32_t mclw_hal_spi_sink_max_clock_hz(const struct mclw_spi_sink *s) {
    (void)s;
    return 0;
}
