// SPDX-License-Identifier: Apache-2.0 OR MIT
// I2C targets for the host port, served per transaction from the bridge:
// the emulated DUT's controller writes bytes, reads bytes and ends the
// transaction, and the target answers from its image or sink exactly as
// the rp2 port's engine does (metalclaw/hal/i2c_target.h). Bus events
// arrive in order here, so none of that engine's handling of an interrupt
// taken late is needed. Several targets may share one bus at different
// addresses.

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"

#include "host.h"

#define I2C_TARGETS_MAX 8u
#define I2C_SINK_RING 256u

enum open_kind {
    OPEN_NONE,
    OPEN_WRITE,
    OPEN_READ,
};

struct mclw_i2c_target {
    uint8_t sda;
    uint8_t scl;
    uint8_t address;
    const void *owner;
    struct mclw_regmap *image;
    uint8_t pointer;
    uint8_t selected;
    bool pointer_pending;
    uint8_t open;
    struct mclw_i2c_target_stats stats;
    struct mclw_sink *sink;
    bool sink_ended;
    bool sink_open;
    uint32_t head;
    uint32_t tail;
    uint16_t ring[I2C_SINK_RING];
};

static struct mclw_i2c_target targets[I2C_TARGETS_MAX];
static bool poll_registered;

// Bridged, a burst of bus bytes all comes within one request's handling or
// a few, with no worker round between them; a sink's bytes go to the
// model when its ring is a quarter full and at each transaction's end.
#define SINK_DELIVER_AT (I2C_SINK_RING / 4u)

static void poll_sinks(void *ctx);
static struct mclw_work sink_work = { .fn = NULL, .ctx = NULL, .pending = false, .next = NULL };

static void sink_work_fn(struct mclw_work *work) {
    (void)work;
    poll_sinks(NULL);
}

static void sink_due(void) {
    sink_work.fn = sink_work_fn;
    mclw_hal_work_post(&sink_work);
}

static struct mclw_i2c_target *find(uint8_t sda, uint8_t scl, uint8_t address) {
    for (size_t i = 0; i < I2C_TARGETS_MAX; i++) {
        struct mclw_i2c_target *t = &targets[i];

        if (t->owner != NULL && t->sda == sda && t->scl == scl && t->address == address) {
            return t;
        }
    }
    return NULL;
}

static void end_transaction(struct mclw_i2c_target *t) {
    if (t->sink != NULL) {
        t->sink_ended = true;
        sink_due();
    } else {
        if (t->image != NULL) {
            t->pointer = mclw_regmap_bus_restart(t->image, t->selected, t->pointer);
        }
        t->pointer_pending = true;
    }
    t->open = OPEN_NONE;
    t->stats.transactions++;
}

static void receive(struct mclw_i2c_target *t, uint8_t byte, bool first) {
    struct mclw_regmap *image = t->image;

    if (first && t->open != OPEN_NONE) {
        end_transaction(t);
    }
    t->open = OPEN_WRITE;
    t->stats.bytes++;
    if (t->sink != NULL) {
        if (t->head - t->tail >= I2C_SINK_RING) {
            t->stats.dropped++;
        } else {
            t->ring[t->head % I2C_SINK_RING] = (uint16_t)(byte | (first ? MCLW_SINK_FIRST : 0u));
            t->head++;
            if (t->head - t->tail >= SINK_DELIVER_AT) {
                sink_due();
            }
        }
        t->sink_ended = false;
        return;
    }
    if (image != NULL && mclw_regmap_bus_input(image, byte)) {
        t->pointer_pending = false;
    } else if (t->pointer_pending) {
        t->selected = image != NULL ? mclw_regmap_bus_select(image, byte) : byte;
        t->pointer = t->selected;
        t->pointer_pending = false;
    } else if (image != NULL) {
        mclw_regmap_bus_write(image, t->pointer++, byte);
    }
}

// The bus each DUT controller is on, and the target its last START
// addressed, which the bytes after it go to until a STOP.
struct bus {
    uint8_t sda;
    uint8_t scl;
    struct mclw_i2c_target *target;
    bool first;
};

#define I2C_BUSES_MAX 8u

static struct bus buses[I2C_BUSES_MAX];

static struct bus *bus_for(uint8_t sda, uint8_t scl) {
    struct bus *free_bus = NULL;

    for (size_t i = 0; i < I2C_BUSES_MAX; i++) {
        if (buses[i].sda == sda && buses[i].scl == scl && (buses[i].sda | buses[i].scl) != 0u) {
            return &buses[i];
        }
        if (free_bus == NULL && buses[i].sda == 0u && buses[i].scl == 0u) {
            free_bus = &buses[i];
        }
    }
    if (free_bus != NULL) {
        free_bus->sda = sda;
        free_bus->scl = scl;
        free_bus->target = NULL;
    }
    return free_bus;
}

// A START, or a repeated START: whatever transaction the bus was in ends
// there, on the target it addressed.
bool mclw_host_i2c_start(uint8_t sda, uint8_t scl, uint8_t address, bool read) {
    uint32_t state = mclw_hal_critical_enter();
    struct bus *b = bus_for(sda, scl);
    struct mclw_i2c_target *t = find(sda, scl, address);

    if (b != NULL && b->target != NULL && b->target->open != OPEN_NONE) {
        end_transaction(b->target);
    }
    if (b != NULL) {
        b->target = t;
        b->first = true;
    }
    if (t != NULL && read) {
        t->open = OPEN_READ;
    }
    mclw_hal_critical_exit(state);
    return t != NULL;
}

bool mclw_host_i2c_write(uint8_t sda, uint8_t scl, uint8_t byte) {
    uint32_t state = mclw_hal_critical_enter();
    struct bus *b = bus_for(sda, scl);
    struct mclw_i2c_target *t = b != NULL ? b->target : NULL;

    if (t != NULL) {
        receive(t, byte, b->first);
        b->first = false;
    }
    mclw_hal_critical_exit(state);
    return t != NULL;
}

uint8_t mclw_host_i2c_read(uint8_t sda, uint8_t scl) {
    uint32_t state = mclw_hal_critical_enter();
    struct bus *b = bus_for(sda, scl);
    struct mclw_i2c_target *t = b != NULL ? b->target : NULL;
    uint8_t byte = 0xff;

    if (t != NULL) {
        t->open = OPEN_READ;
        if (t->sink != NULL) {
            byte = t->sink->read_byte;
        } else if (t->image != NULL) {
            byte = mclw_regmap_bus_read(t->image, t->pointer++);
        }
        t->stats.bytes++;
    }
    mclw_hal_critical_exit(state);
    return byte;
}

void mclw_host_i2c_stop(uint8_t sda, uint8_t scl) {
    uint32_t state = mclw_hal_critical_enter();
    struct bus *b = bus_for(sda, scl);

    if (b != NULL && b->target != NULL) {
        if (b->target->open != OPEN_NONE) {
            end_transaction(b->target);
        }
        b->target = NULL;
    }
    mclw_hal_critical_exit(state);
}

static void sink_deliver(struct mclw_i2c_target *t) {
    for (;;) {
        uint32_t state = mclw_hal_critical_enter();
        struct mclw_sink *sink = t->sink;
        uint32_t head = t->head;
        uint32_t tail = t->tail;
        bool ended = t->sink_ended;

        mclw_hal_critical_exit(state);
        if (sink == NULL) {
            return;
        }
        uint32_t pending = head - tail;
        if (pending == 0u) {
            if (ended && t->sink_open) {
                t->sink_open = false;
                sink->on_bytes(sink, t->ring, 0, true, sink->ctx);
            }
            return;
        }
        uint32_t at = tail % I2C_SINK_RING;
        uint32_t count = pending < I2C_SINK_RING - at ? pending : I2C_SINK_RING - at;
        bool last = count == pending;

        sink->on_bytes(sink, &t->ring[at], count, last && ended, sink->ctx);
        t->sink_open = !(last && ended);
        state = mclw_hal_critical_enter();
        t->tail = tail + count;
        mclw_hal_critical_exit(state);
    }
}

static void poll_sinks(void *ctx) {
    (void)ctx;
    if (!mclw_hal_try_lock()) {
        return;
    }
    for (size_t i = 0; i < I2C_TARGETS_MAX; i++) {
        if (targets[i].owner != NULL && targets[i].sink != NULL) {
            sink_deliver(&targets[i]);
        }
    }
    mclw_hal_unlock();
}

struct mclw_i2c_target *mclw_hal_i2c_target_open(uint8_t sda, uint8_t scl, uint8_t address,
    const void *owner, int *err) {
    struct mclw_i2c_target *t = NULL;

    if (sda == scl || address > 0x7f || owner == NULL) {
        *err = -EINVAL;
        return NULL;
    }
    uint32_t state = mclw_hal_critical_enter();
    if (find(sda, scl, address) != NULL) {
        mclw_hal_critical_exit(state);
        *err = -EBUSY;
        return NULL;
    }
    for (size_t i = 0; i < I2C_TARGETS_MAX && t == NULL; i++) {
        if (targets[i].owner == NULL) {
            t = &targets[i];
        }
    }
    mclw_hal_critical_exit(state);
    if (t == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    // Targets on one bus share its two lines, so one owner holds them;
    // another owner's target on the same bus finds them claimed and is
    // refused, as on a harness, which answers one address per bus.
    *err = mclw_hal_pin_claim(sda, owner);
    if (*err == 0) {
        *err = mclw_hal_pin_claim(scl, owner);
        if (*err != 0) {
            mclw_hal_pin_release(sda, owner);
        }
    }
    if (*err != 0) {
        return NULL;
    }
    state = mclw_hal_critical_enter();
    memset(t, 0, sizeof(*t));
    t->sda = sda;
    t->scl = scl;
    t->address = address;
    t->pointer_pending = true;
    t->owner = owner;
    mclw_hal_critical_exit(state);
    return t;
}

void mclw_hal_i2c_target_close(struct mclw_i2c_target *t) {
    if (t == NULL || t->owner == NULL) {
        return;
    }
    const void *owner = t->owner;
    uint32_t state = mclw_hal_critical_enter();

    t->owner = NULL;
    t->image = NULL;
    t->sink = NULL;
    for (size_t i = 0; i < I2C_BUSES_MAX; i++) {
        if (buses[i].target == t) {
            buses[i].target = NULL;
        }
    }
    mclw_hal_critical_exit(state);
    mclw_hal_pin_release(t->sda, owner);
    mclw_hal_pin_release(t->scl, owner);
}

int mclw_hal_i2c_target_arm(struct mclw_i2c_target *t, struct mclw_regmap *image) {
    if (t == NULL || t->owner == NULL) {
        return -EINVAL;
    }
    uint32_t state = mclw_hal_critical_enter();
    t->sink = NULL;
    t->image = image;
    // Pointer value 0 is selected, as at a part's power-on (DS25095A
    // register 5-1); it takes effect only for an image with a pointer map.
    if (image != NULL) {
        t->selected = mclw_regmap_bus_select(image, 0);
        t->pointer = mclw_regmap_bus_restart(image, t->selected, t->pointer);
    }
    t->pointer_pending = true;
    t->open = OPEN_NONE;
    mclw_hal_critical_exit(state);
    return 0;
}

int mclw_hal_i2c_target_arm_sink(struct mclw_i2c_target *t, struct mclw_sink *sink) {
    if (t == NULL || t->owner == NULL || sink == NULL || sink->on_bytes == NULL) {
        return -EINVAL;
    }
    if (!poll_registered) {
        if (mclw_hal_poll_register(poll_sinks, NULL) != 0) {
            return -ENOSPC;
        }
        poll_registered = true;
    }
    uint32_t state = mclw_hal_critical_enter();
    t->image = NULL;
    t->head = 0;
    t->tail = 0;
    t->open = OPEN_NONE;
    t->sink_ended = false;
    t->sink_open = false;
    t->sink = sink;
    mclw_hal_critical_exit(state);
    return 0;
}

// Bridged, the controller's transactions arrive whole and the target holds
// no line: a transaction the reset cut short just ends.
void mclw_hal_i2c_target_release(struct mclw_i2c_target *t) {
    if (t == NULL || t->owner == NULL) {
        return;
    }
    uint32_t state = mclw_hal_critical_enter();
    if (t->open != OPEN_NONE) {
        end_transaction(t);
    }
    mclw_hal_critical_exit(state);
}

void mclw_hal_i2c_target_disarm(struct mclw_i2c_target *t) {
    (void)mclw_hal_i2c_target_arm(t, NULL);
}

void mclw_hal_i2c_target_stats(const struct mclw_i2c_target *t, struct mclw_i2c_target_stats *out) {
    uint32_t state = mclw_hal_critical_enter();
    *out = t->stats;
    mclw_hal_critical_exit(state);
}
