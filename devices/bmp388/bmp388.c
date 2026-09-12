// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "bmp388.h"

enum {
    PROP_PRESSURE,
    PROP_TEMPERATURE,
    PROP_TRANSACTIONS,
    PROP_POWER_MODE,
    PROP_OSR,
    PROP_ODR,
};

enum {
    BINDING_I2C,
};

#define BMP388_RAW_MAX 0xffffff

struct bmp388_state {
    struct metalclaw_regmap map;
    struct metalclaw_i2c_target *i2c;
    int32_t pressure_pa;
    int32_t temperature_mc;
};

static const struct metalclaw_property_desc bmp388_properties[] = {
    {
        .name = "pressure",
        .type = METALCLAW_TYPE_I32,
        .count = 1,
        .access = METALCLAW_ACCESS_RW,
        .unit = "Pa",
        .min = BMP388_PRESSURE_MIN_PA,
        .max = BMP388_PRESSURE_MAX_PA,
    },
    {
        .name = "temperature",
        .type = METALCLAW_TYPE_I32,
        .count = 1,
        .access = METALCLAW_ACCESS_RW,
        .unit = "mC",
        .min = BMP388_TEMPERATURE_MIN_MC,
        .max = BMP388_TEMPERATURE_MAX_MC,
    },
    {
        .name = "transactions",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "power_mode",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = BMP388_PWR_CTRL_MODE_MASK,
    },
    {
        .name = "osr",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = BMP388_OSR_MASK,
    },
    {
        .name = "odr",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = BMP388_ODR_MASK,
    },
};

static const struct metalclaw_binding_desc bmp388_bindings[] = {
    {
        .name = "i2c",
        .kind = METALCLAW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
    },
};

static uint32_t clamp_raw(int64_t value) {
    if (value < 0) {
        return 0;
    }
    if (value > BMP388_RAW_MAX) {
        return BMP388_RAW_MAX;
    }
    return (uint32_t)value;
}

uint32_t metalclaw_bmp388_raw_pressure(int32_t pressure_pa) {
    int64_t millipascal = (int64_t)pressure_pa * 1000;
    int64_t raw = (millipascal * BMP388_PRESSURE_NUMERATOR + BMP388_PRESSURE_DENOMINATOR / 2)
        / BMP388_PRESSURE_DENOMINATOR;

    return clamp_raw(raw);
}

uint32_t metalclaw_bmp388_raw_temperature(int32_t temperature_mc) {
    int64_t offset = ((int64_t)temperature_mc * BMP388_TEMPERATURE_NUMERATOR)
        / BMP388_TEMPERATURE_DENOMINATOR;

    return clamp_raw((int64_t)BMP388_NVM_PAR_T1 * 256 + offset);
}

static void set_le16(struct metalclaw_regmap *map, uint8_t reg, uint16_t value) {
    metalclaw_regmap_set(map, 0, reg, (uint8_t)value, 0);
    metalclaw_regmap_set(map, 0, (uint8_t)(reg + 1u), (uint8_t)(value >> 8), 0);
}

static void set_le24(struct metalclaw_regmap *map, uint8_t reg, uint32_t value) {
    metalclaw_regmap_set(map, 0, reg, (uint8_t)value, 0);
    metalclaw_regmap_set(map, 0, (uint8_t)(reg + 1u), (uint8_t)(value >> 8), 0);
    metalclaw_regmap_set(map, 0, (uint8_t)(reg + 2u), (uint8_t)(value >> 16), 0);
}

static void build_calibration(struct metalclaw_regmap *map) {
    set_le16(map, BMP388_REG_NVM_PAR_T1, BMP388_NVM_PAR_T1);
    set_le16(map, BMP388_REG_NVM_PAR_T2, BMP388_NVM_PAR_T2);
    metalclaw_regmap_set(map, 0, BMP388_REG_NVM_PAR_T3, 0, 0);
    set_le16(map, BMP388_REG_NVM_PAR_P1, (uint16_t)BMP388_NVM_PAR_P1);
    set_le16(map, BMP388_REG_NVM_PAR_P2, (uint16_t)BMP388_NVM_PAR_P2);
    for (uint8_t reg = BMP388_REG_NVM_PAR_P3; reg <= BMP388_REG_NVM_PAR_P11; reg++) {
        metalclaw_regmap_set(map, 0, reg, 0, 0);
    }
}

static void build_map(struct bmp388_state *state) {
    struct metalclaw_regmap *map = &state->map;

    metalclaw_regmap_init(map, 1, 0, 0x00);
    metalclaw_regmap_set(map, 0, BMP388_REG_CHIP_ID, BMP388_CHIP_ID, 0);
    metalclaw_regmap_set(map, 0, BMP388_REG_ERR, BMP388_ERR_RESET, 0);
    metalclaw_regmap_set(map, 0, BMP388_REG_STATUS,
        BMP388_STATUS_CMD_RDY | BMP388_STATUS_DRDY_PRESS | BMP388_STATUS_DRDY_TEMP, 0);
    metalclaw_regmap_set(map, 0, BMP388_REG_EVENT, BMP388_EVENT_POR_DETECTED, 0);
    metalclaw_regmap_set(map, 0, BMP388_REG_INT_STATUS, BMP388_INT_STATUS_DRDY,
        METALCLAW_REG_CLEAR_ON_READ);
    metalclaw_regmap_set(map, 0, BMP388_REG_FIFO_WTM_0, BMP388_FIFO_WTM_0_RESET,
        METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, BMP388_REG_FIFO_WTM_1, 0, METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, BMP388_REG_FIFO_CONFIG_1, BMP388_FIFO_CONFIG_RESET,
        METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, BMP388_REG_FIFO_CONFIG_2, BMP388_FIFO_CONFIG_RESET,
        METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, BMP388_REG_INT_CTRL, BMP388_INT_CTRL_RESET,
        METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, BMP388_REG_IF_CONF, 0, METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, BMP388_REG_PWR_CTRL, 0, METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, BMP388_REG_OSR, BMP388_OSR_RESET, METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, BMP388_REG_ODR, 0, METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, BMP388_REG_CONFIG, 0, METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, BMP388_REG_CMD, 0, METALCLAW_REG_WRITABLE);
    build_calibration(map);
}

static void publish_sample(struct metalclaw_instance *inst) {
    struct bmp388_state *state = inst->state;
    uint32_t pressure = metalclaw_bmp388_raw_pressure(state->pressure_pa);
    uint32_t temperature = metalclaw_bmp388_raw_temperature(state->temperature_mc);
    uint32_t critical = metalclaw_hal_critical_enter();

    set_le24(&state->map, BMP388_REG_DATA_0, pressure);
    set_le24(&state->map, BMP388_REG_DATA_0 + 3, temperature);
    metalclaw_regmap_set(&state->map, 0, BMP388_REG_STATUS,
        BMP388_STATUS_CMD_RDY | BMP388_STATUS_DRDY_PRESS | BMP388_STATUS_DRDY_TEMP, 0);
    metalclaw_regmap_set(&state->map, 0, BMP388_REG_INT_STATUS, BMP388_INT_STATUS_DRDY,
        METALCLAW_REG_CLEAR_ON_READ);
    metalclaw_hal_critical_exit(critical);

    metalclaw_trace(inst, METALCLAW_TRACE_STATE, BMP388_REG_DATA_0, BMP388_SAMPLE_BYTES, 0);
}

static void publish_part(struct metalclaw_instance *inst) {
    struct bmp388_state *state = inst->state;

    metalclaw_hal_i2c_target_disarm(state->i2c);
    build_map(state);
    publish_sample(inst);
    metalclaw_hal_i2c_target_arm(state->i2c, &state->map);
}

static int bmp388_init(struct metalclaw_instance *inst, const struct metalclaw_binding *bindings) {
    struct bmp388_state *state = inst->state;

    if (!bindings[BINDING_I2C].present) {
        return -EINVAL;
    }
    state->i2c = bindings[BINDING_I2C].i2c;
    state->pressure_pa = BMP388_PRESSURE_DEFAULT_PA;
    state->temperature_mc = BMP388_TEMPERATURE_DEFAULT_MC;
    publish_part(inst);
    return 0;
}

static void bmp388_deinit(struct metalclaw_instance *inst) {
    struct bmp388_state *state = inst->state;

    metalclaw_hal_i2c_target_disarm(state->i2c);
    state->i2c = NULL;
}

static int bmp388_reset(struct metalclaw_instance *inst, bool factory) {
    struct bmp388_state *state = inst->state;

    (void)factory;
    state->pressure_pa = BMP388_PRESSURE_DEFAULT_PA;
    state->temperature_mc = BMP388_TEMPERATURE_DEFAULT_MC;
    publish_part(inst);
    return 0;
}

static int bmp388_get(struct metalclaw_instance *inst, size_t property,
    struct metalclaw_value *out) {
    struct bmp388_state *state = inst->state;

    switch (property) {
        case PROP_PRESSURE:
            out->v[0] = state->pressure_pa;
            return 0;
        case PROP_TEMPERATURE:
            out->v[0] = state->temperature_mc;
            return 0;
        case PROP_TRANSACTIONS:
            out->v[0] = metalclaw_hal_i2c_target_transactions(state->i2c);
            return 0;
        case PROP_POWER_MODE:
            out->v[0] = (metalclaw_regmap_get(&state->map, 0, BMP388_REG_PWR_CTRL)
                >> BMP388_PWR_CTRL_MODE_POS) & BMP388_PWR_CTRL_MODE_MASK;
            return 0;
        case PROP_OSR:
            out->v[0] = metalclaw_regmap_get(&state->map, 0, BMP388_REG_OSR) & BMP388_OSR_MASK;
            return 0;
        case PROP_ODR:
            out->v[0] = metalclaw_regmap_get(&state->map, 0, BMP388_REG_ODR) & BMP388_ODR_MASK;
            return 0;
        default:
            return -EINVAL;
    }
}

static int bmp388_set(struct metalclaw_instance *inst, size_t property,
    const struct metalclaw_value *in) {
    struct bmp388_state *state = inst->state;

    switch (property) {
        case PROP_PRESSURE:
            state->pressure_pa = (int32_t)in->v[0];
            break;
        case PROP_TEMPERATURE:
            state->temperature_mc = (int32_t)in->v[0];
            break;
        default:
            return -EINVAL;
    }
    publish_sample(inst);
    return 0;
}

static const struct metalclaw_model_ops bmp388_ops = {
    .init = bmp388_init,
    .deinit = bmp388_deinit,
    .reset = bmp388_reset,
    .get = bmp388_get,
    .set = bmp388_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .prepare = NULL,
    .time_advanced = NULL,
    .usb_rx = NULL,
};

const struct metalclaw_model metalclaw_model_bmp388 = {
    .abi_version = METALCLAW_ABI_VERSION,
    .id = "bmp388",
    .family = "bmp388",
    .properties = bmp388_properties,
    .property_count = sizeof(bmp388_properties) / sizeof(bmp388_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = bmp388_bindings,
    .binding_count = sizeof(bmp388_bindings) / sizeof(bmp388_bindings[0]),
    .state_size = sizeof(struct bmp388_state),
    .ops = &bmp388_ops,
};
