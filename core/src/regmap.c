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

// Keeps a stream's lead in step with the register it copies.
static inline void MCLW_RAM_FUNC(mirror)(struct mclw_regmap *map, uint8_t bank, uint8_t reg) {
    if (map->has_stream && bank == map->stream_bank && reg < map->stream_reg &&
        reg >= map->stream_reg - map->stream_lead) {
        map->window[reg - (map->stream_reg - map->stream_lead)] = map->value[bank][reg];
    }
}

void MCLW_RAM_FUNC(mclw_regmap_set)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value, uint8_t flags) {
    if (bank >= map->bank_count) {
        return;
    }
    map->value[bank][reg] = value;
    map->flags[bank][reg] = flags;
    mirror(map, bank, reg);
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

void mclw_regmap_set_pointer_map(struct mclw_regmap *map, const uint8_t *addresses) {
    map->pointer_map = addresses;
}

void mclw_regmap_set_on_input(struct mclw_regmap *map, mclw_regmap_on_input_t fn, void *ctx) {
    map->on_input = fn;
    map->on_input_ctx = ctx;
}

uint8_t MCLW_RAM_FUNC(mclw_regmap_bus_select)(const struct mclw_regmap *map, uint8_t pointer) {
    if (map->on_input != NULL) {
        return 0;
    }
    return map->pointer_map != NULL ? map->pointer_map[pointer] : pointer;
}

uint8_t MCLW_RAM_FUNC(mclw_regmap_bus_restart)(const struct mclw_regmap *map, uint8_t selected, uint8_t next) {
    if (map->on_input != NULL) {
        return 0;
    }
    return map->pointer_map != NULL ? selected : next;
}

bool MCLW_RAM_FUNC(mclw_regmap_bus_input)(struct mclw_regmap *map, uint8_t value) {
    if (map->on_input == NULL) {
        return false;
    }
    map->on_input(map, value, map->on_input_ctx);
    return true;
}

static inline uint8_t MCLW_RAM_FUNC(read_one)(struct mclw_regmap *map, uint8_t reg) {
    uint8_t bank = map->active_bank;
    uint8_t flags = map->flags[bank][reg];
    uint8_t value = map->value[bank][reg];
    if (flags & MCLW_REG_CLEAR_ON_READ) {
        map->value[bank][reg] = 0;
    } else if (flags & MCLW_REG_CLEAR_BIT_ON_READ) {
        map->value[bank][reg] = (uint8_t)(value & ~(1u << (flags >> 5)));
    }
    mirror(map, bank, reg);
    return value;
}

static inline void MCLW_RAM_FUNC(write_one)(struct mclw_regmap *map, uint8_t reg, uint8_t value) {
    uint8_t bank = map->active_bank;
    if (map->bank_count > 1 && reg == map->bank_select_reg) {
        map->active_bank = value < map->bank_count ? value : 0;
    } else if (map->flags[bank][reg] & MCLW_REG_WRITABLE) {
        map->value[bank][reg] = value;
        mirror(map, bank, reg);
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

void mclw_regmap_set_stream(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t lead, uint8_t *window,
    size_t size, mclw_regmap_on_stream_t fn, void *ctx) {
    map->has_stream = false;
    map->stream_bank = bank;
    map->stream_reg = reg;
    map->stream_lead = lead <= reg ? lead : reg;
    map->window = window;
    map->stream_size = size;
    map->on_stream = fn;
    map->on_stream_ctx = ctx;
    if (window != NULL && bank < map->bank_count) {
        map->has_stream = true;
        for (uint8_t r = (uint8_t)(reg - map->stream_lead); r < reg; r++) {
            mirror(map, bank, r);
        }
    }
}

const uint8_t *MCLW_RAM_FUNC(mclw_regmap_stream_from)(const struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    size_t *bytes) {
    uint8_t first;

    if (!map->has_stream || bank != map->stream_bank) {
        return NULL;
    }
    first = (uint8_t)(map->stream_reg - map->stream_lead);
    if (reg < first || reg > map->stream_reg) {
        return NULL;
    }
    *bytes = (size_t)(map->stream_reg - reg) + map->stream_size;
    return map->window + (reg - first);
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
    size_t streamed = 0;
    size_t bytes;

    if (mclw_regmap_stream_from(map, bank, reg, &bytes) != NULL && (increment || reg == map->stream_reg)) {
        size_t lead = (size_t)(map->stream_reg - reg);

        if (count > lead) {
            streamed = count - lead;
            count = lead;
        }
    }
    for (; count > 0; count--) {
        if (flags[reg] & MCLW_REG_CLEAR_ON_READ) {
            value[reg] = 0;
            mirror(map, bank, reg);
        } else if (flags[reg] & MCLW_REG_CLEAR_BIT_ON_READ) {
            value[reg] = (uint8_t)(value[reg] & ~(1u << (flags[reg] >> 5)));
            mirror(map, bank, reg);
        }
        reg = (uint8_t)(reg + step);
    }
    if (streamed != 0u && map->on_stream != NULL) {
        map->on_stream(map, streamed < map->stream_size ? streamed : map->stream_size, map->on_stream_ctx);
    }
}

void MCLW_RAM_FUNC(mclw_regmap_bus_write_burst)(struct mclw_regmap *map, uint8_t reg,
    const uint8_t *values, size_t count, bool increment) {
    for (size_t i = 0; i < count; i++) {
        write_one(map, reg, values[i]);
        reg = (uint8_t)(reg + (increment ? 1u : 0u));
    }
}
