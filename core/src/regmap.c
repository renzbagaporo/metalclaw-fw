// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <string.h>

#include "metalclaw/hal/platform.h"
#include "metalclaw/regmap.h"

void mclw_regmap_init(struct mclw_regmap *map, uint8_t bank_count, uint8_t bank_select_reg, uint8_t filler) {
    memset(map, 0, sizeof(*map));
    map->bank_count = bank_count == 0 ? 1 : bank_count;
    if (map->bank_count > MCLW_REGMAP_BANKS_MAX) {
        map->bank_count = MCLW_REGMAP_BANKS_MAX;
    }
    map->bank_select_reg = bank_select_reg;
    map->filler = filler;
    memset(map->value, filler, sizeof(map->value));
}

void MCLW_RAM_FUNC(mclw_regmap_set)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value, uint8_t flags) {
    if (bank >= map->bank_count) {
        return;
    }
    map->value[bank][reg] = value;
    map->flags[bank][reg] = flags;
}

uint8_t MCLW_RAM_FUNC(mclw_regmap_get)(const struct mclw_regmap *map, uint8_t bank, uint8_t reg) {
    return bank < map->bank_count ? map->value[bank][reg] : map->filler;
}

void MCLW_RAM_FUNC(mclw_regmap_set_le16)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint16_t value) {
    mclw_regmap_set(map, bank, reg, (uint8_t)value, 0);
    mclw_regmap_set(map, bank, (uint8_t)(reg + 1u), (uint8_t)(value >> 8), 0);
}

void MCLW_RAM_FUNC(mclw_regmap_set_le24)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint32_t value) {
    mclw_regmap_set(map, bank, reg, (uint8_t)value, 0);
    mclw_regmap_set(map, bank, (uint8_t)(reg + 1u), (uint8_t)(value >> 8), 0);
    mclw_regmap_set(map, bank, (uint8_t)(reg + 2u), (uint8_t)(value >> 16), 0);
}

void MCLW_RAM_FUNC(mclw_regmap_set_be16)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint16_t value) {
    mclw_regmap_set(map, bank, reg, (uint8_t)(value >> 8), 0);
    mclw_regmap_set(map, bank, (uint8_t)(reg + 1u), (uint8_t)value, 0);
}

void mclw_regmap_set_on_write(struct mclw_regmap *map, mclw_regmap_on_write_t fn, void *ctx) {
    map->on_write = fn;
    map->on_write_ctx = ctx;
}

static inline uint8_t MCLW_RAM_FUNC(read_one)(struct mclw_regmap *map, uint8_t reg) {
    uint8_t bank = map->active_bank;
    uint8_t flags = map->flags[bank][reg];
    uint8_t value = map->value[bank][reg];
    if (flags & MCLW_REG_CLEAR_ON_READ) {
        map->value[bank][reg] = 0;
    }
    return value;
}

static inline void MCLW_RAM_FUNC(write_one)(struct mclw_regmap *map, uint8_t reg, uint8_t value) {
    uint8_t bank = map->active_bank;
    if (map->bank_count > 1 && reg == map->bank_select_reg) {
        map->active_bank = value < map->bank_count ? value : 0;
    } else if (map->flags[bank][reg] & MCLW_REG_WRITABLE) {
        map->value[bank][reg] = value;
    } else {
        return;
    }
    if (map->on_write != NULL) {
        map->on_write(map, bank, reg, value, map->on_write_ctx);
    }
}

uint8_t MCLW_RAM_FUNC(mclw_regmap_bus_read)(struct mclw_regmap *map, uint8_t reg) {
    return read_one(map, reg);
}

void MCLW_RAM_FUNC(mclw_regmap_bus_write)(struct mclw_regmap *map, uint8_t reg, uint8_t value) {
    write_one(map, reg, value);
}

void MCLW_RAM_FUNC(mclw_regmap_bus_read_burst)(struct mclw_regmap *map, uint8_t reg, size_t count,
    bool increment) {
    // A read never moves the bank, so the bank's rows are found once; the
    // byte stores below could alias active_bank and would make the compiler
    // reload it per byte. Only each read's side effect is left to do.
    uint8_t bank = map->active_bank;
    const uint8_t *flags = map->flags[bank];
    uint8_t *value = map->value[bank];
    uint8_t step = increment ? 1u : 0u;

    for (; count > 0; count--) {
        if (flags[reg] & MCLW_REG_CLEAR_ON_READ) {
            value[reg] = 0;
        }
        reg = (uint8_t)(reg + step);
    }
}

void MCLW_RAM_FUNC(mclw_regmap_bus_write_burst)(struct mclw_regmap *map, uint8_t reg,
    const uint8_t *values, size_t count, bool increment) {
    for (size_t i = 0; i < count; i++) {
        write_one(map, reg, values[i]);
        reg = (uint8_t)(reg + (increment ? 1u : 0u));
    }
}
