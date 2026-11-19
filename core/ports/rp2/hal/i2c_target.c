// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stddef.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/sync.h"
#include "pico/i2c_slave.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"

#define I2C_TARGET_CLOCK_HZ 400000
// A sink's captured bytes wait here for the worker's poll, which runs after
// every interrupt the worker takes, each byte's included: at 1 MHz the
// ring holds about 2.5 ms of traffic.
#define I2C_SINK_RING 256u

struct mclw_i2c_target {
    i2c_inst_t *i2c;
    uint8_t sda;
    uint8_t scl;
    uint8_t address;
    const void *owner;
    struct mclw_regmap *image;
    // The image address the master's first written byte selects, through
    // the image's pointer map; auto-incremented after use, and put back to
    // `selected` at the end of a transaction when the image has a map.
    uint8_t pointer;
    uint8_t selected;
    bool pointer_pending;
    struct mclw_i2c_target_stats stats;
    // With a sink armed instead of an image: the interrupt writes entries
    // at `head`, the worker's poll takes them from `tail`.
    struct mclw_sink *sink;
    // The transaction the bytes so far belong to.
    uint8_t open;
    bool sink_ended;
    bool sink_open;
    volatile uint32_t head;
    volatile uint32_t tail;
    uint16_t ring[I2C_SINK_RING];
};

enum open_kind {
    OPEN_NONE,
    OPEN_WRITE,
    OPEN_READ,
};

static struct mclw_i2c_target targets[2];
static bool poll_registered;

static void handler(i2c_inst_t *i2c, i2c_slave_event_t event);

static void slave_start(void *ctx) {
    struct mclw_i2c_target *t = ctx;

    i2c_slave_init(t->i2c, t->address, handler);
}

static void slave_stop(void *ctx) {
    struct mclw_i2c_target *t = ctx;

    i2c_slave_deinit(t->i2c);
}

static i2c_inst_t *instance_for(uint8_t sda, uint8_t scl) {
    // GPIO function I2C maps even pins to SDA and odd to SCL; the block is
    // (pin / 2) % 2, the same for both pins of one bus.
    if (sda >= NUM_BANK0_GPIOS || scl >= NUM_BANK0_GPIOS || (sda & 1u) != 0 || (scl & 1u) != 1 || ((sda / 2) & 1u) != ((scl / 2) & 1u)) {
        return NULL;
    }
    return ((sda / 2) & 1u) ? i2c1 : i2c0;
}

// The bytes the RX FIFO holds, in bus order. The SDK's interrupt reports a
// STOP or repeated START (I2C_SLAVE_FINISH) before RX_FULL, so an interrupt
// taken late sees a transaction end ahead of that transaction's last bytes,
// with the next transaction's first bytes behind them in the FIFO. Each
// entry's FIRST_DATA_BYTE (IC_DATA_CMD bit 11, RP2040 and RP2350
// datasheets) marks where the bus started a write, so transactions are
// split there.
static void receive(struct mclw_i2c_target *t, uint8_t byte, bool first);

static void drain(struct mclw_i2c_target *t, i2c_inst_t *i2c) {
    i2c_hw_t *hw = i2c_get_hw(i2c);

    while (hw->rxflr != 0u) {
        uint32_t entry = hw->data_cmd;
        receive(t, (uint8_t)entry, (entry & I2C_IC_DATA_CMD_FIRST_DATA_BYTE_BITS) != 0u);
    }
}

static void end_transaction(struct mclw_i2c_target *t) {
    if (t->sink != NULL) {
        t->sink_ended = true;
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
        uint32_t head = t->head;

        if (head - t->tail >= I2C_SINK_RING) {
            t->stats.dropped++;
        } else {
            t->ring[head % I2C_SINK_RING] = (uint16_t)(byte | (first ? MCLW_SINK_FIRST : 0u));
            t->head = head + 1u;
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

static void handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    struct mclw_i2c_target *t = &targets[i2c_hw_index(i2c)];
    uint32_t state = mclw_hal_critical_enter();

    drain(t, i2c);
    switch (event) {
        case I2C_SLAVE_RECEIVE:
            break;
        // A read is clock-stretched until it is answered, so its request
        // is never ahead of the bytes before it. A write before it ended
        // with the repeated START.
        case I2C_SLAVE_REQUEST:
            if (t->open == OPEN_WRITE) {
                end_transaction(t);
            }
            t->open = OPEN_READ;
            if (t->sink != NULL) {
                i2c_write_byte_raw(i2c, t->sink->read_byte);
            } else {
                i2c_write_byte_raw(i2c, t->image != NULL ? mclw_regmap_bus_read(t->image, t->pointer++) : 0xff);
            }
            t->stats.bytes++;
            break;
        // A STOP or a repeated START, of the open transaction or, from an
        // interrupt taken late, of one before it that a first byte already
        // ended. A write the target is still in is left open for its
        // own end.
        case I2C_SLAVE_FINISH:
            if (t->open == OPEN_READ || (t->open == OPEN_WRITE && (i2c_get_hw(i2c)->status & I2C_IC_STATUS_SLV_ACTIVITY_BITS) == 0u)) {
                end_transaction(t);
            }
            break;
    }
    mclw_hal_critical_exit(state);
}

// The worker's poll. The entries from tail to head are the interrupt's no
// longer, so the model reads them in place; a window that closed after its
// last entry was delivered is reported on its own.
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
        __dmb();
        t->tail = tail + count;
    }
}

static void poll_sinks(void *ctx) {
    (void)ctx;
    if (!mclw_hal_try_lock()) {
        return;
    }
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        if (targets[i].owner != NULL && targets[i].sink != NULL) {
            sink_deliver(&targets[i]);
        }
    }
    mclw_hal_unlock();
}

struct mclw_i2c_target *mclw_hal_i2c_target_open(uint8_t sda, uint8_t scl, uint8_t address,
    const void *owner, int *err) {
    i2c_inst_t *i2c = instance_for(sda, scl);
    if (i2c == NULL || address > 0x7f || owner == NULL) {
        *err = -EINVAL;
        return NULL;
    }
    struct mclw_i2c_target *t = &targets[i2c_hw_index(i2c)];
    if (t->owner != NULL) {
        *err = -EBUSY;
        return NULL;
    }
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
    t->i2c = i2c;
    t->sda = sda;
    t->scl = scl;
    t->address = address;
    t->owner = owner;
    t->image = NULL;
    t->pointer = 0;
    t->selected = 0;
    t->pointer_pending = true;
    t->open = OPEN_NONE;
    t->stats.transactions = 0;
    t->stats.bytes = 0;
    t->stats.dropped = 0;
    t->sink = NULL;

    gpio_init(sda);
    gpio_init(scl);
    gpio_set_function(sda, GPIO_FUNC_I2C);
    gpio_set_function(scl, GPIO_FUNC_I2C);
    gpio_pull_up(sda);
    gpio_pull_up(scl);
    i2c_init(i2c, I2C_TARGET_CLOCK_HZ);
    mclw_hal_work_call(slave_start, t);
    return t;
}

void mclw_hal_i2c_target_close(struct mclw_i2c_target *t) {
    if (t == NULL || t->owner == NULL) {
        return;
    }
    mclw_hal_work_call(slave_stop, t);
    i2c_deinit(t->i2c);
    gpio_deinit(t->sda);
    gpio_deinit(t->scl);
    mclw_hal_pin_release(t->sda, t->owner);
    mclw_hal_pin_release(t->scl, t->owner);
    t->owner = NULL;
    t->image = NULL;
    t->sink = NULL;
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

void mclw_hal_i2c_target_disarm(struct mclw_i2c_target *t) {
    (void)mclw_hal_i2c_target_arm(t, NULL);
}

// A target mid-transfer waits for SCL edges that a reset controller never
// sends, holding SDA if its bit or acknowledge was a 0; the block's reset
// is what makes it let go. It is set up again as at the open.
void mclw_hal_i2c_target_release(struct mclw_i2c_target *t) {
    if (t == NULL || t->owner == NULL) {
        return;
    }
    if ((i2c_get_hw(t->i2c)->status & I2C_IC_STATUS_SLV_ACTIVITY_BITS) != 0u) {
        mclw_hal_work_call(slave_stop, t);
        i2c_deinit(t->i2c);
        i2c_init(t->i2c, I2C_TARGET_CLOCK_HZ);
        mclw_hal_work_call(slave_start, t);
    }
    uint32_t state = mclw_hal_critical_enter();
    if (t->open != OPEN_NONE) {
        end_transaction(t);
    }
    mclw_hal_critical_exit(state);
}

void mclw_hal_i2c_target_stats(const struct mclw_i2c_target *t, struct mclw_i2c_target_stats *out) {
    uint32_t state = mclw_hal_critical_enter();
    *out = t->stats;
    mclw_hal_critical_exit(state);
}
