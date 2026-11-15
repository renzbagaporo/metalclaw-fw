// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "mcp9808.h"

enum {
    PROP_TEMPERATURE,
    PROP_UPPER,
    PROP_LOWER,
    PROP_CRITICAL,
    PROP_RESOLUTION,
    PROP_CONFIG,
};

enum {
    BINDING_I2C,
};

struct mcp9808_state {
    struct mclw_regmap map;
    uint8_t pointers[MCLW_REGMAP_BANK_SIZE];
    struct mclw_i2c_target *i2c;
    int32_t temperature_mc;
    // The last conversion's temperature; held while in shutdown (5.2.1).
    int32_t converted_mc;
    // Set lock bits, which only a power-on reset clears (register 5-2).
    uint8_t locks;
};

#define LIMIT_PROPERTY(property_name) \
    { \
        .name = (property_name), \
        .type = MCLW_TYPE_I32, \
        .count = 1, \
        .access = MCLW_ACCESS_RO, \
        .unit = "mC", \
        .min = -256000, \
        .max = 255750, \
    }

static const struct mclw_property_desc mcp9808_properties[] = {
    {
        .name = "temperature",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mC",
        .min = MCP9808_TEMPERATURE_MIN_MC,
        .max = MCP9808_TEMPERATURE_MAX_MC,
    },
    LIMIT_PROPERTY("upper"),
    LIMIT_PROPERTY("lower"),
    LIMIT_PROPERTY("critical"),
    {
        .name = "resolution",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = MCP9808_RESOLUTION_MASK,
    },
    {
        .name = "config",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT16_MAX,
    },
};

static const struct mclw_binding_desc mcp9808_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static inline uint8_t byte_of(uint8_t reg) {
    return (uint8_t)(reg * MCP9808_REGISTER_BYTES);
}

static uint16_t MCLW_RAM_FUNC(get_word)(const struct mcp9808_state *state, uint8_t reg) {
    uint8_t at = (uint8_t)(reg * MCP9808_REGISTER_BYTES);

    return (uint16_t)((mclw_regmap_get(&state->map, 0, at) << 8)
        | mclw_regmap_get(&state->map, 0, (uint8_t)(at + 1u)));
}

static void MCLW_RAM_FUNC(set_word)(struct mcp9808_state *state, uint8_t reg, uint16_t value,
    uint8_t flags) {
    uint8_t at = (uint8_t)(reg * MCP9808_REGISTER_BYTES);

    mclw_regmap_set(&state->map, 0, at, (uint8_t)(value >> 8), flags);
    mclw_regmap_set(&state->map, 0, (uint8_t)(at + 1u), (uint8_t)value, flags);
}

static uint8_t MCLW_RAM_FUNC(resolution)(const struct mcp9808_state *state) {
    return mclw_regmap_get(&state->map, 0, (uint8_t)(MCP9808_REG_RESOLUTION * MCP9808_REGISTER_BYTES))
        & MCP9808_RESOLUTION_MASK;
}

// The comparison bits follow every limit write, as the part's comparator
// does between conversions; the hysteresis is not modelled.
static void MCLW_RAM_FUNC(refresh_ta)(struct mcp9808_state *state) {
    set_word(state, MCP9808_REG_TA, mcp9808_ta_word(state->converted_mc, resolution(state),
        get_word(state, MCP9808_REG_UPPER), get_word(state, MCP9808_REG_LOWER),
        get_word(state, MCP9808_REG_CRIT)), 0);
}

static void MCLW_RAM_FUNC(apply_locks)(struct mcp9808_state *state) {
    uint8_t window = (state->locks & MCP9808_CONFIG_WIN_LOCK) != 0 ? 0 : MCLW_REG_WRITABLE;
    uint8_t crit = (state->locks & MCP9808_CONFIG_CRIT_LOCK) != 0 ? 0 : MCLW_REG_WRITABLE;

    set_word(state, MCP9808_REG_UPPER, get_word(state, MCP9808_REG_UPPER), window);
    set_word(state, MCP9808_REG_LOWER, get_word(state, MCP9808_REG_LOWER), window);
    set_word(state, MCP9808_REG_CRIT, get_word(state, MCP9808_REG_CRIT), crit);
}

// Interrupt context, once per byte the engine committed; edits only the
// image and the state, from RAM.
static void MCLW_RAM_FUNC(on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t at,
    uint8_t value, void *ctx) {
    struct mcp9808_state *state = ctx;
    uint8_t reg = (uint8_t)(at / MCP9808_REGISTER_BYTES);
    bool msb = (at % MCP9808_REGISTER_BYTES) == 0;
    (void)bank;

    switch (reg) {
        case MCP9808_REG_CONFIG:
            if (msb) {
                value &= MCP9808_CONFIG_MSB_MASK;
                if ((value & MCP9808_CONFIG_SHDN) == 0) {
                    state->converted_mc = state->temperature_mc;
                }
            } else {
                state->locks |= value & (MCP9808_CONFIG_CRIT_LOCK | MCP9808_CONFIG_WIN_LOCK);
                value = (uint8_t)((value & MCP9808_CONFIG_LSB_MASK) | state->locks);
            }
            mclw_regmap_set(map, 0, at, value, MCLW_REG_WRITABLE);
            if (!msb) {
                apply_locks(state);
            }
            break;
        case MCP9808_REG_UPPER:
        case MCP9808_REG_LOWER:
        case MCP9808_REG_CRIT:
            value &= msb ? (uint8_t)(MCP9808_LIMIT_MASK >> 8) : (uint8_t)MCP9808_LIMIT_MASK;
            mclw_regmap_set(map, 0, at, value, MCLW_REG_WRITABLE);
            break;
        case MCP9808_REG_RESOLUTION:
            mclw_regmap_set(map, 0, at, value & MCP9808_RESOLUTION_MASK, MCLW_REG_WRITABLE);
            break;
        default:
            return;
    }
    refresh_ta(state);
}

// Power-on defaults, DS25095A table 5-3. Pointer p selects image byte 2p;
// the pointers past Resolution select the zero bytes after it.
static void build_map(struct mcp9808_state *state) {
    mclw_regmap_init(&state->map, 1, 0, 0x00);
    for (unsigned p = 0; p < MCLW_REGMAP_BANK_SIZE; p++) {
        state->pointers[p] = byte_of((uint8_t)(p <= MCP9808_REG_RESOLUTION ? p : MCP9808_REG_RESOLUTION + 1u));
    }
    mclw_regmap_set_pointer_map(&state->map, state->pointers);
    set_word(state, MCP9808_REG_RFU, MCP9808_RFU_VALUE, 0);
    set_word(state, MCP9808_REG_CONFIG, 0, MCLW_REG_WRITABLE);
    set_word(state, MCP9808_REG_UPPER, 0, MCLW_REG_WRITABLE);
    set_word(state, MCP9808_REG_LOWER, 0, MCLW_REG_WRITABLE);
    set_word(state, MCP9808_REG_CRIT, 0, MCLW_REG_WRITABLE);
    set_word(state, MCP9808_REG_MANUFACTURER, MCP9808_MANUFACTURER_ID, 0);
    set_word(state, MCP9808_REG_DEVICE, MCP9808_DEVICE_ID, 0);
    mclw_regmap_set(&state->map, 0, byte_of(MCP9808_REG_RESOLUTION), MCP9808_RESOLUTION_RESET,
        MCLW_REG_WRITABLE);
    state->locks = 0;
    state->converted_mc = state->temperature_mc;
    refresh_ta(state);
    mclw_regmap_set_on_write(&state->map, on_write, state);
}

static int power_up(struct mcp9808_state *state) {
    mclw_hal_i2c_target_disarm(state->i2c);
    build_map(state);
    return mclw_hal_i2c_target_arm(state->i2c, &state->map);
}

static int mcp9808_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct mcp9808_state *state = inst->state;

    state->i2c = bindings[BINDING_I2C].i2c;
    state->temperature_mc = MCP9808_TEMPERATURE_DEFAULT_MC;
    return power_up(state);
}

static void mcp9808_deinit(struct mclw_instance *inst) {
    struct mcp9808_state *state = inst->state;

    mclw_hal_i2c_target_disarm(state->i2c);
    state->i2c = NULL;
}

static int mcp9808_reset(struct mclw_instance *inst, bool factory) {
    struct mcp9808_state *state = inst->state;

    (void)factory;
    state->temperature_mc = MCP9808_TEMPERATURE_DEFAULT_MC;
    return power_up(state);
}

static int mcp9808_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct mcp9808_state *state = inst->state;

    switch (property) {
        case PROP_TEMPERATURE:
            out->v[0] = state->temperature_mc;
            return 0;
        case PROP_UPPER:
            out->v[0] = mcp9808_limit_milli_c(get_word(state, MCP9808_REG_UPPER));
            return 0;
        case PROP_LOWER:
            out->v[0] = mcp9808_limit_milli_c(get_word(state, MCP9808_REG_LOWER));
            return 0;
        case PROP_CRITICAL:
            out->v[0] = mcp9808_limit_milli_c(get_word(state, MCP9808_REG_CRIT));
            return 0;
        case PROP_RESOLUTION:
            out->v[0] = resolution(state);
            return 0;
        case PROP_CONFIG:
            out->v[0] = get_word(state, MCP9808_REG_CONFIG);
            return 0;
        default:
            return -EINVAL;
    }
}

// The part converts continuously; the model's conversion is immediate
// rather than every tCONV (table 5-2), and none happens in shutdown.
static int mcp9808_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct mcp9808_state *state = inst->state;
    uint32_t critical;

    if (property != PROP_TEMPERATURE) {
        return -EINVAL;
    }
    critical = mclw_hal_critical_enter();
    state->temperature_mc = (int32_t)in->v[0];
    if ((get_word(state, MCP9808_REG_CONFIG) & (MCP9808_CONFIG_SHDN << 8)) == 0) {
        state->converted_mc = state->temperature_mc;
    }
    refresh_ta(state);
    mclw_hal_critical_exit(critical);
    mclw_trace(inst, MCLW_TRACE_STATE, byte_of(MCP9808_REG_TA), MCP9808_REGISTER_BYTES, 0);
    return 0;
}

static const struct mclw_model_ops mcp9808_ops = {
    .init = mcp9808_init,
    .deinit = mcp9808_deinit,
    .reset = mcp9808_reset,
    .get = mcp9808_get,
    .set = mcp9808_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .usb_rx = NULL,
};

const struct mclw_model mclw_model_mcp9808 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "mcp9808",
    .family = "mcp9808",
    .config = NULL,
    .properties = mcp9808_properties,
    .property_count = sizeof(mcp9808_properties) / sizeof(mcp9808_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = mcp9808_bindings,
    .binding_count = sizeof(mcp9808_bindings) / sizeof(mcp9808_bindings[0]),
    .state_size = sizeof(struct mcp9808_state),
    .ops = &mcp9808_ops,
};
