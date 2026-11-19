// SPDX-License-Identifier: Apache-2.0 OR MIT
// The harness as an I2C controller on a hardware I2C block (the RP2040's
// and RP2350's DesignWare DW_apb_i2c, as a master): the worker feeds the
// block's command FIFO a byte at a time, a read command per byte to read,
// RESTART on the first read after a write and STOP on the last command,
// and drains the receive FIFO as bytes arrive. The block holds SCL low
// while its command FIFO is empty before a STOP (RP2040 datasheet 4.3.10.2,
// IC_EMPTYFIFO_HOLD_MASTER_EN), so a slow worker pass stretches the bus
// rather than ending the transfer. An address or data NACK aborts the
// transfer (TX_ABRT, its cause in IC_TX_ABRT_SOURCE) and the block sends
// the STOP itself; a target that holds SCL past MCLW_I2C_STRETCH_MAX_US is
// cut off with IC_ENABLE.ABORT.

#include <errno.h>
#include <stddef.h>

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/i2c.h"

#include "metalclaw/hal/i2c_controller.h"
#include "metalclaw/hal/platform.h"

#include "gpio_internal.h"

#define FIFO_DEPTH 16u

struct mclw_i2c_controller {
    bool open;
    i2c_inst_t *i2c;
    struct mclw_gpio *sda;
    struct mclw_gpio *scl;
    uint32_t hz;
    // The transfer in flight: what to send and where reads go, how many
    // commands went into the FIFO and how many bytes came back.
    volatile bool busy;
    const uint8_t *out;
    size_t wlen;
    uint8_t *in;
    size_t rlen;
    size_t fed;
    size_t received;
    size_t last_progress;
    uint64_t progress_us;
    bool aborting;
    enum mclw_i2c_result result;
    size_t moved;
};

static struct mclw_i2c_controller controllers[2];
static bool poll_registered;

static void finish(struct mclw_i2c_controller *c, enum mclw_i2c_result result, size_t moved) {
    c->result = result;
    c->moved = moved;
    c->busy = false;
}

static void serve(struct mclw_i2c_controller *c) {
    i2c_hw_t *hw = i2c_get_hw(c->i2c);
    size_t total = c->wlen + c->rlen;
    uint32_t raw = hw->raw_intr_stat;

    if ((raw & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) != 0u) {
        uint32_t source = hw->tx_abrt_source;
        size_t moved = c->fed > hw->txflr ? c->fed - hw->txflr : 0u;

        (void)hw->clr_tx_abrt;
        (void)hw->clr_stop_det;
        if ((source & I2C_IC_TX_ABRT_SOURCE_ABRT_USER_ABRT_BITS) != 0u || c->aborting) {
            finish(c, MCLW_I2C_STRETCH_TIMEOUT, moved);
        } else if ((source & I2C_IC_TX_ABRT_SOURCE_ABRT_7B_ADDR_NOACK_BITS) != 0u) {
            finish(c, MCLW_I2C_ADDRESS_NACK, 0u);
        } else {
            // The byte refused is the last one counted as moved.
            finish(c, MCLW_I2C_DATA_NACK, moved != 0u ? moved - 1u : 0u);
        }
        return;
    }
    while (c->received < c->rlen && hw->rxflr != 0u) {
        c->in[c->received++] = (uint8_t)hw->data_cmd;
    }
    while (c->fed < total && hw->txflr < FIFO_DEPTH) {
        size_t k = c->fed;
        uint32_t cmd;

        if (k < c->wlen) {
            cmd = c->out[k];
        } else {
            // A read command waits for room for its byte.
            if (k - c->wlen - c->received >= FIFO_DEPTH) {
                break;
            }
            cmd = I2C_IC_DATA_CMD_CMD_BITS;
            if (k == c->wlen && c->wlen != 0u) {
                cmd |= I2C_IC_DATA_CMD_RESTART_BITS;
            }
        }
        if (k + 1u == total) {
            cmd |= I2C_IC_DATA_CMD_STOP_BITS;
        }
        hw->data_cmd = cmd;
        c->fed++;
    }
    if ((raw & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS) != 0u && c->fed == total && c->received == c->rlen) {
        (void)hw->clr_stop_det;
        finish(c, MCLW_I2C_OK, total);
        return;
    }
    // Progress is a command taken or a byte received; none for the
    // stretch limit while SCL is low is a target holding it.
    size_t progress = (c->fed - hw->txflr) + c->received;
    uint64_t now = mclw_hal_time_us();

    if (progress != c->last_progress) {
        c->last_progress = progress;
        c->progress_us = now;
    } else if (!c->aborting && now - c->progress_us > MCLW_I2C_STRETCH_MAX_US && !gpio_get(c->scl->pin)) {
        c->aborting = true;
        hw->enable |= I2C_IC_ENABLE_ABORT_BITS;
    }
}

static void poll_controllers(void *ctx) {
    (void)ctx;
    for (size_t i = 0; i < sizeof(controllers) / sizeof(controllers[0]); i++) {
        struct mclw_i2c_controller *c = &controllers[i];

        if (c->open && c->busy) {
            serve(c);
        }
    }
}

static i2c_inst_t *instance_for(uint8_t sda, uint8_t scl) {
    // GPIO function I2C maps even pins to SDA and odd to SCL; the block is
    // (pin / 2) % 2, the same for both pins of one bus.
    if (sda >= NUM_BANK0_GPIOS || scl >= NUM_BANK0_GPIOS || (sda & 1u) != 0 || (scl & 1u) != 1 ||
        ((sda / 2) & 1u) != ((scl / 2) & 1u)) {
        return NULL;
    }
    return ((sda / 2) & 1u) ? i2c1 : i2c0;
}

struct mclw_i2c_controller *mclw_hal_i2c_controller_open(struct mclw_gpio *sda, struct mclw_gpio *scl, uint32_t hz,
    int *err) {
    i2c_inst_t *i2c = sda != NULL && scl != NULL ? instance_for(sda->pin, scl->pin) : NULL;

    if (i2c == NULL || hz == 0u) {
        *err = -EINVAL;
        return NULL;
    }
    struct mclw_i2c_controller *c = &controllers[i2c_hw_index(i2c)];

    // A block that is enabled is some target's.
    if (c->open || (i2c_get_hw(i2c)->enable & I2C_IC_ENABLE_ENABLE_BITS) != 0u) {
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
    *c = (struct mclw_i2c_controller){ .open = true, .i2c = i2c, .sda = sda, .scl = scl, .hz = hz };
    sda->held = true;
    scl->held = true;
    i2c_init(i2c, hz);
    gpio_set_function(sda->pin, GPIO_FUNC_I2C);
    gpio_set_function(scl->pin, GPIO_FUNC_I2C);
    gpio_pull_up(sda->pin);
    gpio_pull_up(scl->pin);
    return c;
}

static void stop_on_worker(void *ctx) {
    struct mclw_i2c_controller *c = ctx;

    c->busy = false;
}

void mclw_hal_i2c_controller_close(struct mclw_i2c_controller *c) {
    if (c == NULL || !c->open) {
        return;
    }
    mclw_hal_work_call(stop_on_worker, c);
    i2c_deinit(c->i2c);
    c->sda->held = false;
    c->scl->held = false;
    mclw_rp2_gpio_restore(c->sda);
    mclw_rp2_gpio_restore(c->scl);
    c->open = false;
    bool any = false;
    for (size_t i = 0; i < sizeof(controllers) / sizeof(controllers[0]); i++) {
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
    c->hz = hz;
    i2c_set_baudrate(c->i2c, hz);
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
    if (!gpio_get(c->sda->pin) || !gpio_get(c->scl->pin)) {
        c->result = MCLW_I2C_BUS_HELD;
        c->moved = 0u;
        return 0;
    }
    i2c_hw_t *hw = i2c_get_hw(c->i2c);

    hw->enable = 0;
    hw->tar = address;
    hw->enable = I2C_IC_ENABLE_ENABLE_BITS;
    (void)hw->clr_intr;
    c->out = out;
    c->wlen = wlen;
    c->in = in;
    c->rlen = rlen;
    c->fed = 0u;
    c->received = 0u;
    c->last_progress = 0u;
    c->progress_us = mclw_hal_time_us();
    c->aborting = false;
    c->moved = 0u;
    __dmb();
    c->busy = true;
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
