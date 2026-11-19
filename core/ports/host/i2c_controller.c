// SPDX-License-Identifier: Apache-2.0 OR MIT
// I2C controllers for the host port, toward the emulated DUT's target
// (BRIDGE.md i2c_drive): a transfer is a sequence of events, START and the
// address, each written byte, a repeated START and the address to read,
// each read byte, STOP, and the next event goes out only once the last
// was answered, a byte time later at the controller's rate, so the DUT's
// firmware gets the time it has on a wire, and its clock stretching shows
// as a later answer. No answer within MCLW_I2C_STRETCH_MAX_US of virtual
// time ends the transfer with a STOP.

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/i2c_controller.h"
#include "metalclaw/hal/platform.h"

#include "host.h"

#define CONTROLLERS_MAX 2u

struct mclw_i2c_controller {
    bool open;
    uint8_t sda;
    uint8_t scl;
    uint64_t byte_ns;
    // The transfer: what to write and where reads go, the next event's
    // index, the event out and when it went, and how it is ending.
    volatile bool busy;
    uint8_t address;
    const uint8_t *out;
    size_t wlen;
    uint8_t *in;
    size_t rlen;
    size_t next;
    bool waiting;
    uint8_t sent;
    uint64_t sent_ns;
    bool stopping;
    enum mclw_i2c_result result;
    size_t moved;
};

static struct mclw_i2c_controller controllers[CONTROLLERS_MAX];
static bool poll_registered;

// Event k of the transfer: [START W, WRITE x wlen] [START R, READ x rlen] STOP.
static uint8_t event_at(const struct mclw_i2c_controller *c, size_t k, uint8_t *arg, bool *flag) {
    size_t write_events = c->wlen != 0u ? 1u + c->wlen : 0u;
    size_t read_events = c->rlen != 0u ? 1u + c->rlen : 0u;

    *arg = 0u;
    *flag = false;
    if (k < write_events) {
        if (k == 0u) {
            *arg = c->address;
            return MCLW_HOST_I2C_START;
        }
        *arg = c->out[k - 1u];
        return MCLW_HOST_I2C_WRITE;
    }
    k -= write_events;
    if (k < read_events) {
        if (k == 0u) {
            *arg = c->address;
            *flag = true;
            return MCLW_HOST_I2C_START;
        }
        // Every byte but the last is acknowledged.
        *flag = k < c->rlen;
        return MCLW_HOST_I2C_READ;
    }
    return MCLW_HOST_I2C_STOP;
}

static void send(struct mclw_i2c_controller *c, uint8_t ev, uint8_t arg, bool flag, uint64_t t_ns) {
    c->sent = ev;
    c->sent_ns = t_ns;
    c->waiting = mclw_host_event_i2c_drive(c->sda, c->scl, ev, arg, flag, t_ns);
}

static void send_next(struct mclw_i2c_controller *c, uint64_t t_ns) {
    uint8_t arg;
    bool flag;
    uint8_t ev = event_at(c, c->next++, &arg, &flag);

    if (ev == MCLW_HOST_I2C_STOP) {
        c->stopping = true;
    }
    send(c, ev, arg, flag, t_ns);
}

static void stop_now(struct mclw_i2c_controller *c, enum mclw_i2c_result result, uint64_t t_ns) {
    c->result = result;
    c->stopping = true;
    send(c, MCLW_HOST_I2C_STOP, 0u, false, t_ns);
}

void mclw_host_i2c_driven(uint8_t sda, uint8_t scl, int ack, int data, uint64_t t_ns) {
    for (size_t i = 0; i < CONTROLLERS_MAX; i++) {
        struct mclw_i2c_controller *c = &controllers[i];

        if (!c->open || !c->busy || !c->waiting || c->sda != sda || c->scl != scl) {
            continue;
        }
        c->waiting = false;
        if (c->sent == MCLW_HOST_I2C_STOP) {
            c->busy = false;
            return;
        }
        uint64_t next_ns = t_ns + c->byte_ns;

        if (c->sent == MCLW_HOST_I2C_START && ack == 0) {
            stop_now(c, MCLW_I2C_ADDRESS_NACK, next_ns);
        } else if (c->sent == MCLW_HOST_I2C_WRITE && ack == 0) {
            stop_now(c, MCLW_I2C_DATA_NACK, next_ns);
        } else {
            if (c->sent == MCLW_HOST_I2C_WRITE) {
                c->moved++;
            } else if (c->sent == MCLW_HOST_I2C_READ) {
                size_t r = c->moved - c->wlen;

                if (r < c->rlen) {
                    c->in[r] = data >= 0 ? (uint8_t)data : 0xffu;
                }
                c->moved++;
            }
            send_next(c, next_ns);
        }
        return;
    }
}

// A target that leaves an event unanswered past the stretch limit: STOP.
static void poll_controllers(void *ctx) {
    (void)ctx;
    uint64_t now = mclw_host_event_ns();

    for (size_t i = 0; i < CONTROLLERS_MAX; i++) {
        struct mclw_i2c_controller *c = &controllers[i];

        if (!c->open || !c->busy || !c->waiting || now < c->sent_ns ||
            now - c->sent_ns <= (uint64_t)MCLW_I2C_STRETCH_MAX_US * 1000u) {
            continue;
        }
        if (c->stopping) {
            c->waiting = false;
            c->busy = false;
        } else {
            stop_now(c, MCLW_I2C_STRETCH_TIMEOUT, now);
        }
    }
}

struct mclw_i2c_controller *mclw_hal_i2c_controller_open(struct mclw_gpio *sda, struct mclw_gpio *scl, uint32_t hz,
    int *err) {
    struct mclw_i2c_controller *c = NULL;

    if (sda == NULL || scl == NULL || hz == 0u) {
        *err = -EINVAL;
        return NULL;
    }
    for (size_t i = 0; i < CONTROLLERS_MAX && c == NULL; i++) {
        if (!controllers[i].open) {
            c = &controllers[i];
        }
    }
    if (c == NULL) {
        *err = -EBUSY;
        return NULL;
    }
    if (!poll_registered) {
        if (mclw_hal_poll_register(poll_controllers, NULL) != 0) {
            *err = -ENOSPC;
            return NULL;
        }
        poll_registered = true;
    }
    *c = (struct mclw_i2c_controller){ .open = true, .sda = mclw_hal_gpio_pin(sda), .scl = mclw_hal_gpio_pin(scl),
        .byte_ns = 9000000000ull / hz };
    return c;
}

void mclw_hal_i2c_controller_close(struct mclw_i2c_controller *c) {
    if (c == NULL || !c->open) {
        return;
    }
    c->open = false;
    c->busy = false;
    bool any = false;
    for (size_t i = 0; i < CONTROLLERS_MAX; i++) {
        any = any || controllers[i].open;
    }
    if (!any && poll_registered) {
        mclw_hal_poll_unregister(poll_controllers, NULL);
        poll_registered = false;
    }
}

int mclw_hal_i2c_controller_set_rate(struct mclw_i2c_controller *c, uint32_t hz) {
    if (c == NULL || !c->open || hz == 0u) {
        return -EINVAL;
    }
    if (c->busy) {
        return -EBUSY;
    }
    c->byte_ns = 9000000000ull / hz;
    return 0;
}

int mclw_hal_i2c_controller_transfer(struct mclw_i2c_controller *c, uint8_t address, const uint8_t *out, size_t wlen,
    uint8_t *in, size_t rlen) {
    if (c == NULL || !c->open || address > 0x7fu || (wlen == 0u && rlen == 0u) || (wlen != 0u && out == NULL) ||
        (rlen != 0u && in == NULL)) {
        return -EINVAL;
    }
    if (c->busy) {
        return -EBUSY;
    }
    c->address = address;
    c->out = out;
    c->wlen = wlen;
    c->in = in;
    c->rlen = rlen;
    c->next = 0u;
    c->stopping = false;
    c->result = MCLW_I2C_OK;
    c->moved = 0u;
    c->busy = true;
    send_next(c, mclw_host_event_ns());
    if (!c->waiting) {
        // No bus has these pins: as on a wire with no target there.
        c->busy = false;
        c->result = MCLW_I2C_ADDRESS_NACK;
    }
    return 0;
}

bool mclw_hal_i2c_controller_busy(const struct mclw_i2c_controller *c) {
    return c != NULL && c->busy;
}

enum mclw_i2c_result mclw_hal_i2c_controller_result(const struct mclw_i2c_controller *c, size_t *moved) {
    if (moved != NULL) {
        *moved = c->moved;
    }
    return c->result;
}
