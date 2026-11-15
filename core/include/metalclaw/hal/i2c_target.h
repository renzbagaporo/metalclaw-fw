// SPDX-License-Identifier: Apache-2.0 OR MIT
// An I2C target (slave) that serves a register map from hardware. The bus
// master's first byte after a write START selects the register, through the
// map's pointer map when it has one; further bytes write it with
// auto-increment; reads return registers with auto-increment. Reads and
// writes go through mclw_regmap_bus_* in interrupt context, so the model
// updates its map under a critical section.
//
// When the image has a pointer map, every transaction (after a STOP or a
// repeated START) starts again at the image address its last pointer byte
// selected: a read with no pointer write re-reads the selected register from
// its first byte, as TI's INA2xx (SBOS448G 8.5.6) and Microchip's MCP9808
// (DS25095A 4.1.1) retain their register pointer. Arming such an image
// selects pointer value 0. Without a map the pointer is a byte address that
// runs on from one transaction to the next.
//
// A sink (metalclaw/sink.h) is armed instead for a part with no register
// pointer: every written byte, the first included, is captured with
// MCLW_SINK_FIRST on the first of each transaction, and reads return the
// sink's read_byte.

#ifndef MCLW_HAL_I2C_TARGET_H
#define MCLW_HAL_I2C_TARGET_H

#include <stdint.h>

#include "metalclaw/regmap.h"
#include "metalclaw/sink.h"

struct mclw_i2c_target;

struct mclw_i2c_target_stats {
    uint32_t transactions;
    uint32_t bytes;
    // Bytes a sink lost because the worker had not taken the ones before.
    uint32_t dropped;
};

// Claims sda and scl for `owner` and configures the target at `address`.
// The target answers nothing until armed.
struct mclw_i2c_target *mclw_hal_i2c_target_open(uint8_t sda, uint8_t scl, uint8_t address,
    const void *owner, int *err);
void mclw_hal_i2c_target_close(struct mclw_i2c_target *t);

// `image` must outlive the arm; it is read and written from interrupt
// context. Arming replaces whatever was armed.
int mclw_hal_i2c_target_arm(struct mclw_i2c_target *t, struct mclw_regmap *image);
// The sink must outlive the arm; arming either kind replaces the other.
int mclw_hal_i2c_target_arm_sink(struct mclw_i2c_target *t, struct mclw_sink *sink);
void mclw_hal_i2c_target_disarm(struct mclw_i2c_target *t);

void mclw_hal_i2c_target_stats(const struct mclw_i2c_target *t, struct mclw_i2c_target_stats *out);

#endif
