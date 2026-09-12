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

#ifndef METALCLAW_REGMAP_H
#define METALCLAW_REGMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define METALCLAW_REGMAP_BANKS_MAX 4
#define METALCLAW_REGMAP_BANK_SIZE 256

enum metalclaw_reg_flag {
    METALCLAW_REG_WRITABLE = 1u << 0,
    METALCLAW_REG_CLEAR_ON_READ = 1u << 1,
    // Reads advance a stream instead of returning the register byte.
    METALCLAW_REG_STREAM = 1u << 2,
};

// How the part frames a register access on SPI, from its datasheet: which
// command-byte bit means read, which bits are the address, whether a
// multi-byte access walks consecutive addresses, and how many don't-care
// bytes the part clocks out before read data. Not used on I2C.
struct metalclaw_regmap_framing {
    uint8_t read_bit_mask;
    uint8_t address_mask;
    uint8_t read_dummy_bytes;
    bool auto_increment;
};

struct metalclaw_regmap;

typedef void (*metalclaw_regmap_on_write_t)(struct metalclaw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx);

struct metalclaw_regmap {
    uint8_t bank_count;
    uint8_t bank_select_reg;
    uint8_t active_bank;
    uint8_t filler;
    uint8_t value[METALCLAW_REGMAP_BANKS_MAX][METALCLAW_REGMAP_BANK_SIZE];
    uint8_t flags[METALCLAW_REGMAP_BANKS_MAX][METALCLAW_REGMAP_BANK_SIZE];
    const uint8_t *stream;
    uint32_t stream_len;
    uint32_t stream_pos;
    metalclaw_regmap_on_write_t on_write;
    void *on_write_ctx;
    // Bumped by every model-side set; engines that cache the image compare it.
    volatile uint32_t revision;
};

// Clears the hook as well as the image.
void metalclaw_regmap_init(struct metalclaw_regmap *map, uint8_t bank_count, uint8_t bank_select_reg, uint8_t filler);
void metalclaw_regmap_set(struct metalclaw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value, uint8_t flags);
uint8_t metalclaw_regmap_get(const struct metalclaw_regmap *map, uint8_t bank, uint8_t reg);
void metalclaw_regmap_set_stream(struct metalclaw_regmap *map, const uint8_t *data, uint32_t len);
void metalclaw_regmap_set_on_write(struct metalclaw_regmap *map, metalclaw_regmap_on_write_t fn, void *ctx);

// The engine side: what a bus read or write of `reg` in the active bank does.
// bus_write commits the value (bank select included) and fires on_write.
uint8_t metalclaw_regmap_bus_read(struct metalclaw_regmap *map, uint8_t reg);
void metalclaw_regmap_bus_write(struct metalclaw_regmap *map, uint8_t reg, uint8_t value);

#endif
