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
    // One bit of the byte clears on a read, the rest stay: a status flag
    // that shares its byte with settings (the TMP116's Data_Ready). The
    // bit's number goes in the flag's top three bits; see below.
    MCLW_REG_CLEAR_BIT_ON_READ = 1u << 2,
};
#define MCLW_REG_CLEAR_BIT(n) (MCLW_REG_CLEAR_BIT_ON_READ | ((unsigned)(n) << 5))

struct mclw_regmap;

typedef void (*mclw_regmap_on_write_t)(struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx);
typedef void (*mclw_regmap_on_input_t)(struct mclw_regmap *map, uint8_t value, void *ctx);

struct mclw_regmap {
    uint8_t bank_count;
    uint8_t bank_select_reg;
    uint8_t active_bank;
    uint8_t filler;
    uint8_t value[MCLW_REGMAP_BANKS_MAX][MCLW_REGMAP_BANK_SIZE];
    uint8_t flags[MCLW_REGMAP_BANKS_MAX][MCLW_REGMAP_BANK_SIZE];
    mclw_regmap_on_write_t on_write;
    void *on_write_ctx;
    const uint8_t *pointer_map;
    mclw_regmap_on_input_t on_input;
    void *on_input_ctx;
};

// Clears the hook and the pointer map as well as the image.
void mclw_regmap_init(struct mclw_regmap *map, uint8_t bank_count, uint8_t bank_select_reg, uint8_t filler);
void mclw_regmap_set(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value, uint8_t flags);
uint8_t mclw_regmap_get(const struct mclw_regmap *map, uint8_t bank, uint8_t reg);

// Multi-byte registers at consecutive addresses, read-only, in the byte
// order the datasheet gives.
void mclw_regmap_set_le16(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint16_t value);
void mclw_regmap_set_le24(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint32_t value);
void mclw_regmap_set_be16(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint16_t value);
void mclw_regmap_set_on_write(struct mclw_regmap *map, mclw_regmap_on_write_t fn, void *ctx);

// For a part whose register pointer does not name one byte: registers wider
// than a byte (TI's INA2xx, 16 to 40 bits behind one pointer value), or one
// register at several pointer values (an auto-increment flag in the pointer,
// a FIFO behind an alias). The model lays the registers out in the image and
// gives, for each of the 256 pointer values, the address of the image byte a
// transfer starts at; the transfer then runs on through the image one byte
// per bus byte. Every transaction on a map with a pointer map starts again at
// the address its last pointer byte selected, so a read with no pointer
// write re-reads the selected register from its first byte. The table must
// outlive the map and may be edited under the critical section. NULL, the
// default, makes the pointer the address and lets it run on from one
// transaction to the next. The I2C engine applies it; SPI framing addresses
// the image directly.
void mclw_regmap_set_pointer_map(struct mclw_regmap *map, const uint8_t *addresses);

// For a part with no register pointer at all, a byte stream behind its bus
// address (MediaTek's NMEA port over I2C: GlobalTop's "NMEA over I2C
// Software Guide" 3.1 and 4). Every transaction starts at image address 0,
// and every byte the master writes goes to `fn`, in interrupt context and
// with the same rules as on_write, instead of selecting a register or
// landing in the image. The I2C engine applies it; SPI framing addresses the
// image directly. NULL, the default, restores the register pointer.
void mclw_regmap_set_on_input(struct mclw_regmap *map, mclw_regmap_on_input_t fn, void *ctx);

// The engine side: the image address a pointer value the bus wrote selects;
// where the next transaction starts, given the address the last pointer byte
// selected and the one this transaction would have gone on to; then what a
// bus read or write of `reg` in the active bank does.
uint8_t mclw_regmap_bus_select(const struct mclw_regmap *map, uint8_t pointer);
uint8_t mclw_regmap_bus_restart(const struct mclw_regmap *map, uint8_t selected, uint8_t next);
// bus_input hands a written byte to the map's on_input and says whether it
// took it; a map without one leaves the byte to the pointer and bus_write.
bool mclw_regmap_bus_input(struct mclw_regmap *map, uint8_t value);
// bus_write commits the value (bank select included) and fires on_write.
uint8_t mclw_regmap_bus_read(struct mclw_regmap *map, uint8_t reg);
void mclw_regmap_bus_write(struct mclw_regmap *map, uint8_t reg, uint8_t value);
// A window at once: `count` of the calls above from `reg` on, the register
// one higher each time if `increment` (wrapping within the bank).
void mclw_regmap_bus_read_burst(struct mclw_regmap *map, uint8_t reg, size_t count, bool increment);
void mclw_regmap_bus_write_burst(struct mclw_regmap *map, uint8_t reg, const uint8_t *values,
    size_t count, bool increment);

#endif
