// SPDX-License-Identifier: Apache-2.0 OR MIT
// A register file a bus-target engine serves from hardware. The model
// fills it in and the engine reads it without calling the model. Side
// effects of a read (clear-on-read, FIFO pop) are declared per register so
// the engine can apply them, again without the model.
//
// Writes from the bus land in the image as they are committed (I2C: per
// byte; SPI: at chip-select release). `on_write` then fires once per
// written register, in interrupt context: no allocation, no HAL calls, edit
// the image only. A model that needs more defers to its next thread-context
// entry point.

#ifndef MCLW_REGMAP_H
#define MCLW_REGMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MCLW_REGMAP_BANKS_MAX 4
#define MCLW_REGMAP_BANK_SIZE 256

enum mclw_reg_flag {
    MCLW_REG_WRITABLE = 1u << 0,
    MCLW_REG_CLEAR_ON_READ = 1u << 1,
};

struct mclw_regmap;

typedef void (*mclw_regmap_on_write_t)(struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx);

struct mclw_regmap {
    uint8_t bank_count;
    uint8_t bank_select_reg;
    uint8_t active_bank;
    uint8_t filler;
    uint8_t value[MCLW_REGMAP_BANKS_MAX][MCLW_REGMAP_BANK_SIZE];
    uint8_t flags[MCLW_REGMAP_BANKS_MAX][MCLW_REGMAP_BANK_SIZE];
    mclw_regmap_on_write_t on_write;
    void *on_write_ctx;
};

// Clears the hook as well as the image.
void mclw_regmap_init(struct mclw_regmap *map, uint8_t bank_count, uint8_t bank_select_reg, uint8_t filler);
void mclw_regmap_set(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value, uint8_t flags);
uint8_t mclw_regmap_get(const struct mclw_regmap *map, uint8_t bank, uint8_t reg);

// Multi-byte registers at consecutive addresses, read-only, in the byte
// order the datasheet gives.
void mclw_regmap_set_le16(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint16_t value);
void mclw_regmap_set_le24(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint32_t value);
void mclw_regmap_set_be16(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint16_t value);
void mclw_regmap_set_on_write(struct mclw_regmap *map, mclw_regmap_on_write_t fn, void *ctx);

// The engine side: what a bus read or write of `reg` in the active bank does.
// bus_write commits the value (bank select included) and fires on_write.
uint8_t mclw_regmap_bus_read(struct mclw_regmap *map, uint8_t reg);
void mclw_regmap_bus_write(struct mclw_regmap *map, uint8_t reg, uint8_t value);
// A window at once: `count` of the calls above from `reg` on, the register
// one higher each time if `increment` (wrapping within the bank).
void mclw_regmap_bus_read_burst(struct mclw_regmap *map, uint8_t reg, size_t count, bool increment);
void mclw_regmap_bus_write_burst(struct mclw_regmap *map, uint8_t reg, const uint8_t *values,
    size_t count, bool increment);

#endif
