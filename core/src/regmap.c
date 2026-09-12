// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <string.h>

#include "metalclaw/regmap.h"

void metalclaw_regmap_init(struct metalclaw_regmap *map, uint8_t bank_count, uint8_t bank_select_reg, uint8_t filler) {
    memset(map, 0, sizeof(*map));
    map->bank_count = bank_count == 0 ? 1 : bank_count;
    if (map->bank_count > METALCLAW_REGMAP_BANKS_MAX) {
        map->bank_count = METALCLAW_REGMAP_BANKS_MAX;
    }
    map->bank_select_reg = bank_select_reg;
    map->filler = filler;
    memset(map->value, filler, sizeof(map->value));
}

void metalclaw_regmap_set(struct metalclaw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value, uint8_t flags) {
    if (bank >= map->bank_count) {
        return;
    }
    map->value[bank][reg] = value;
    map->flags[bank][reg] = flags;
}

uint8_t metalclaw_regmap_get(const struct metalclaw_regmap *map, uint8_t bank, uint8_t reg) {
    return bank < map->bank_count ? map->value[bank][reg] : map->filler;
}

void metalclaw_regmap_set_on_write(struct metalclaw_regmap *map, metalclaw_regmap_on_write_t fn, void *ctx) {
    map->on_write = fn;
    map->on_write_ctx = ctx;
}

uint8_t metalclaw_regmap_bus_read(struct metalclaw_regmap *map, uint8_t reg) {
    uint8_t bank = map->active_bank;
    uint8_t flags = map->flags[bank][reg];
    uint8_t value = map->value[bank][reg];
    if (flags & METALCLAW_REG_CLEAR_ON_READ) {
        map->value[bank][reg] = 0;
    }
    return value;
}

void metalclaw_regmap_bus_write(struct metalclaw_regmap *map, uint8_t reg, uint8_t value) {
    uint8_t bank = map->active_bank;
    if (map->bank_count > 1 && reg == map->bank_select_reg) {
        map->active_bank = value < map->bank_count ? value : 0;
    } else if (map->flags[bank][reg] & METALCLAW_REG_WRITABLE) {
        map->value[bank][reg] = value;
    } else {
        return;
    }
    if (map->on_write != NULL) {
        map->on_write(map, bank, reg, value, map->on_write_ctx);
    }
}
