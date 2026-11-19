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
// A part with no register pointer at all takes one of two forms:
//   - an image with an on_input hook (metalclaw/regmap.h), for a part whose
//     reads carry data: every written byte goes to the hook in interrupt
//     context, one at a time and with no transaction boundaries, and every
//     transaction reads from image address 0;
//   - a sink (metalclaw/sink.h), for a part that only listens: every
//     written byte is captured, MCLW_SINK_FIRST on the first of each
//     transaction, and handed to the model on the worker with the
//     transaction's end; reads return the sink's read_byte.
// A target holds one image or one sink; arming either replaces the other.

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
// Lets go of the bus as the part would at its power-on: a transfer the
// controller left part way (its reset landing mid-byte) ends, and a target
// that was holding SDA for it releases the line. What is armed stays.
void mclw_hal_i2c_target_release(struct mclw_i2c_target *t);

void mclw_hal_i2c_target_stats(const struct mclw_i2c_target *t, struct mclw_i2c_target_stats *out);

#endif
