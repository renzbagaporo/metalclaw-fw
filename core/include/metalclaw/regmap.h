// SPDX-License-Identifier: Apache-2.0 OR MIT
// A register file a bus-target engine serves from hardware. The model
// fills it in prepare(); the engine reads it without calling the model.
// Side effects of a read (clear-on-read, FIFO pop) are declared per
// register so the engine can apply them, again without the model.

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
};

void metalclaw_regmap_init(struct metalclaw_regmap *map, uint8_t bank_count, uint8_t bank_select_reg, uint8_t filler);
void metalclaw_regmap_set(struct metalclaw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value, uint8_t flags);
uint8_t metalclaw_regmap_get(const struct metalclaw_regmap *map, uint8_t bank, uint8_t reg);
void metalclaw_regmap_set_stream(struct metalclaw_regmap *map, const uint8_t *data, uint32_t len);

// The engine side: what a bus read or write of `reg` in the active bank does.
uint8_t metalclaw_regmap_bus_read(struct metalclaw_regmap *map, uint8_t reg);
void metalclaw_regmap_bus_write(struct metalclaw_regmap *map, uint8_t reg, uint8_t value);

#endif
