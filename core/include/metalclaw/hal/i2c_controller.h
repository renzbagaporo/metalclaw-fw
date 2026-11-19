// SPDX-License-Identifier: Apache-2.0 OR MIT
// The harness as an I2C controller on two pins a model holds as gpio
// bindings, for a DUT that is the target: 7-bit addressing, a write, a
// read, or a write then a repeated START and a read, each transfer ended
// by a STOP. The DUT may stretch the clock. A transfer runs on the worker;
// the model starts one and polls busy().

#ifndef MCLW_HAL_I2C_CONTROLLER_H
#define MCLW_HAL_I2C_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mclw_gpio;
struct mclw_i2c_controller;

// How a transfer ended.
enum mclw_i2c_result {
    MCLW_I2C_OK = 0,
    // No target acknowledged the address.
    MCLW_I2C_ADDRESS_NACK = 1,
    // The target refused a written byte.
    MCLW_I2C_DATA_NACK = 2,
    // SDA or SCL was low when the transfer was to start.
    MCLW_I2C_BUS_HELD = 3,
    // The target held SCL low longer than MCLW_I2C_STRETCH_MAX_US.
    MCLW_I2C_STRETCH_TIMEOUT = 4,
};

#define MCLW_I2C_STRETCH_MAX_US 25000u

// The two pins must be an SDA and SCL pair the port can drive as a
// controller (on rp2, one hardware I2C block's pair, a block no target
// holds). -EINVAL for a pair it cannot, -EBUSY for a block in use.
struct mclw_i2c_controller *mclw_hal_i2c_controller_open(struct mclw_gpio *sda, struct mclw_gpio *scl, uint32_t hz,
    int *err);
void mclw_hal_i2c_controller_close(struct mclw_i2c_controller *c);
// -EBUSY while a transfer runs.
int mclw_hal_i2c_controller_set_rate(struct mclw_i2c_controller *c, uint32_t hz);

// Writes `wlen` bytes of `out` to `address`, then, when `rlen` is not 0,
// reads `rlen` bytes into `in` (after a repeated START if anything was
// written), acknowledging every one but the last; a STOP ends it. At least
// one of the two lengths is not 0. Both buffers stay valid until busy() is
// false. -EBUSY while one runs.
int mclw_hal_i2c_controller_transfer(struct mclw_i2c_controller *c, uint8_t address, const uint8_t *out, size_t wlen,
    uint8_t *in, size_t rlen);
bool mclw_hal_i2c_controller_busy(const struct mclw_i2c_controller *c);
// The last transfer's end, and how many of its bytes (written then read)
// went through before it.
enum mclw_i2c_result mclw_hal_i2c_controller_result(const struct mclw_i2c_controller *c, size_t *moved);

#endif
