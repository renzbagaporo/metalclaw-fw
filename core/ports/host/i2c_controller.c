// SPDX-License-Identifier: Apache-2.0 OR MIT
// I2C controllers for the host port: none yet. A transfer toward the
// emulated DUT's target needs the bridge's harness-driven I2C events,
// which are not in the protocol (BRIDGE.md), so opening one is refused.

#include <errno.h>
#include <stddef.h>

#include "metalclaw/hal/i2c_controller.h"

struct mclw_i2c_controller *mclw_hal_i2c_controller_open(struct mclw_gpio *sda, struct mclw_gpio *scl, uint32_t hz,
    int *err) {
    (void)sda;
    (void)scl;
    (void)hz;
    *err = -ENOTSUP;
    return NULL;
}

void mclw_hal_i2c_controller_close(struct mclw_i2c_controller *c) {
    (void)c;
}

int mclw_hal_i2c_controller_set_rate(struct mclw_i2c_controller *c, uint32_t hz) {
    (void)c;
    (void)hz;
    return -ENOTSUP;
}

int mclw_hal_i2c_controller_transfer(struct mclw_i2c_controller *c, uint8_t address, const uint8_t *out, size_t wlen,
    uint8_t *in, size_t rlen) {
    (void)c;
    (void)address;
    (void)out;
    (void)wlen;
    (void)in;
    (void)rlen;
    return -ENOTSUP;
}

bool mclw_hal_i2c_controller_busy(const struct mclw_i2c_controller *c) {
    (void)c;
    return false;
}

enum mclw_i2c_result mclw_hal_i2c_controller_result(const struct mclw_i2c_controller *c, size_t *moved) {
    (void)c;
    if (moved != NULL) {
        *moved = 0u;
    }
    return MCLW_I2C_OK;
}
