// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/bus.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "icm4268x.h"

enum {
    PROP_ACCEL,
    PROP_GYRO,
    PROP_TEMPERATURE,
    PROP_SPI_MODE,
    PROP_POWER_MODE,
    PROP_ACCEL_CONFIG,
    PROP_GYRO_CONFIG,
    PROP_BANK,
    PROP_RESETS,
};

enum {
    BINDING_SPI,
};

struct icm4268x_config {
    uint8_t who_am_i;
};

struct icm4268x_state {
    struct metalclaw_regmap map;
    struct metalclaw_spi_target *spi;
    uint8_t who_am_i;
    int16_t accel[3];
    int16_t gyro[3];
    int16_t temperature;
    uint32_t resets;
};

static const struct metalclaw_property_desc icm4268x_properties[] = {
    {
        .name = "accel",
        .type = METALCLAW_TYPE_I32,
        .count = 3,
        .access = METALCLAW_ACCESS_RW,
        .unit = "LSB",
        .min = INT16_MIN,
        .max = INT16_MAX,
    },
    {
        .name = "gyro",
        .type = METALCLAW_TYPE_I32,
        .count = 3,
        .access = METALCLAW_ACCESS_RW,
        .unit = "LSB",
        .min = INT16_MIN,
        .max = INT16_MAX,
    },
    {
        .name = "temperature",
        .type = METALCLAW_TYPE_I32,
        .count = 1,
        .access = METALCLAW_ACCESS_RW,
        .unit = "LSB",
        .min = INT16_MIN,
        .max = INT16_MAX,
    },
    METALCLAW_PROPERTY_SPI_MODE,
    {
        .name = "power_mode",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 0xff,
    },
    {
        .name = "accel_config",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 0xff,
    },
    {
        .name = "gyro_config",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 0xff,
    },
    {
        .name = "bank",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = ICM4268X_BANKS - 1,
    },
    {
        .name = "resets",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
};

static const struct metalclaw_binding_desc icm4268x_bindings[] = {
    {
        .name = "spi",
        .kind = METALCLAW_BINDING_SPI_TARGET,
        .required = true,
        .usb = NULL,
        .spi = {
            .modes = METALCLAW_SPI_MODE_BIT(0) | METALCLAW_SPI_MODE_BIT(3),
            .default_mode = 3,
            .msb_first = true,
        },
        .uart = { 0 },
    },
};

static const struct metalclaw_regmap_framing icm4268x_framing = {
    .read_bit_mask = ICM4268X_SPI_READ_BIT,
    .address_mask = ICM4268X_SPI_ADDRESS_MASK,
    .read_dummy_bytes = 0,
    .auto_increment = true,
};

static void load_sample(struct icm4268x_state *state) {
    metalclaw_regmap_set_be16(&state->map, 0, ICM4268X_REG_TEMP_DATA1, (uint16_t)state->temperature);
    for (uint8_t i = 0; i < 3; i++) {
        metalclaw_regmap_set_be16(&state->map, 0, (uint8_t)(ICM4268X_REG_ACCEL_DATA_X1 + 2 * i), (uint16_t)state->accel[i]);
        metalclaw_regmap_set_be16(&state->map, 0, (uint8_t)(ICM4268X_REG_GYRO_DATA_X1 + 2 * i), (uint16_t)state->gyro[i]);
    }
}

// Power-on state, as the part leaves it after a soft reset: every register
// writable and zero except the few with a documented reset value.
static void build_map(struct icm4268x_state *state) {
    struct metalclaw_regmap *map = &state->map;

    for (uint8_t bank = 0; bank < ICM4268X_BANKS; bank++) {
        memset(map->value[bank], 0, METALCLAW_REGMAP_BANK_SIZE);
        memset(map->flags[bank], METALCLAW_REG_WRITABLE, METALCLAW_REGMAP_BANK_SIZE);
    }
    map->active_bank = 0;
    metalclaw_regmap_set(map, 0, ICM4268X_REG_WHO_AM_I, state->who_am_i, 0);
    metalclaw_regmap_set(map, 0, ICM4268X_REG_INT_STATUS, ICM4268X_INT_STATUS_RESET_DONE, 0);
    metalclaw_regmap_set(map, 0, ICM4268X_REG_TMST_CONFIG, ICM4268X_TMST_CONFIG_RESET, METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, ICM4268X_REG_INTF_CONFIG1, ICM4268X_INTF_CONFIG1_RESET, METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, ICM4268X_REG_INT_CONFIG1, ICM4268X_INT_CONFIG1_RESET, METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, ICM4268X_REG_ACCEL_CONFIG0, ICM4268X_ACCEL_CONFIG0_RESET, METALCLAW_REG_WRITABLE);
    metalclaw_regmap_set(map, 0, ICM4268X_REG_GYRO_CONFIG0, ICM4268X_GYRO_CONFIG0_RESET, METALCLAW_REG_WRITABLE);
    for (uint8_t reg = ICM4268X_REG_TEMP_DATA1; reg < ICM4268X_REG_TEMP_DATA1 + ICM4268X_SAMPLE_BYTES; reg++) {
        metalclaw_regmap_set(map, 0, reg, 0, 0);
    }
    load_sample(state);
}

static void raise_data_ready(struct icm4268x_state *state) {
    struct metalclaw_regmap *map = &state->map;
    if (metalclaw_regmap_get(map, 0, ICM4268X_REG_PWR_MGMT0) & ICM4268X_PWR_MGMT0_SENSOR_MASK) {
        metalclaw_regmap_set(map, 0, ICM4268X_REG_INT_STATUS,
            metalclaw_regmap_get(map, 0, ICM4268X_REG_INT_STATUS) | ICM4268X_INT_STATUS_DATA_RDY, 0);
    }
}

// Interrupt context, after the engine committed a bus write.
static void on_write(struct metalclaw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value, void *ctx) {
    struct icm4268x_state *state = ctx;
    (void)map;

    if (bank != 0) {
        return;
    }
    if (reg == ICM4268X_REG_DEVICE_CONFIG && (value & ICM4268X_DEVICE_CONFIG_SOFT_RESET)) {
        build_map(state);
        state->resets++;
        metalclaw_trace(metalclaw_instance_from_state(state), METALCLAW_TRACE_COMMAND, reg, 1, 0);
    } else if (reg == ICM4268X_REG_PWR_MGMT0) {
        raise_data_ready(state);
    }
}

static void publish_sample(struct metalclaw_instance *inst) {
    struct icm4268x_state *state = inst->state;
    uint32_t critical = metalclaw_hal_critical_enter();

    load_sample(state);
    raise_data_ready(state);
    metalclaw_hal_critical_exit(critical);
    metalclaw_trace(inst, METALCLAW_TRACE_STATE, ICM4268X_REG_TEMP_DATA1, ICM4268X_SAMPLE_BYTES, 0);
}

static int icm4268x_init(struct metalclaw_instance *inst, const struct metalclaw_binding *bindings) {
    struct icm4268x_state *state = inst->state;

    state->spi = bindings[BINDING_SPI].spi;
    state->who_am_i = ((const struct icm4268x_config *)inst->model->config)->who_am_i;
    metalclaw_regmap_init(&state->map, ICM4268X_BANKS, ICM4268X_REG_BANK_SEL, 0x00);
    metalclaw_regmap_set_on_write(&state->map, on_write, state);
    build_map(state);
    return metalclaw_hal_spi_target_arm_regmap(state->spi, &state->map, &icm4268x_framing);
}

static void icm4268x_deinit(struct metalclaw_instance *inst) {
    struct icm4268x_state *state = inst->state;

    metalclaw_hal_spi_target_disarm(state->spi);
    state->spi = NULL;
}

static int icm4268x_reset(struct metalclaw_instance *inst, bool factory) {
    struct icm4268x_state *state = inst->state;
    uint32_t critical;

    (void)factory;
    memset(state->accel, 0, sizeof(state->accel));
    memset(state->gyro, 0, sizeof(state->gyro));
    state->temperature = 0;
    critical = metalclaw_hal_critical_enter();
    build_map(state);
    metalclaw_hal_critical_exit(critical);
    return 0;
}

static int icm4268x_get(struct metalclaw_instance *inst, size_t property, struct metalclaw_value *out) {
    struct icm4268x_state *state = inst->state;

    switch (property) {
        case PROP_ACCEL:
        case PROP_GYRO: {
            const int16_t *axes = property == PROP_ACCEL ? state->accel : state->gyro;
            for (size_t i = 0; i < 3; i++) {
                out->v[i] = axes[i];
            }
            return 0;
        }
        case PROP_TEMPERATURE:
            out->v[0] = state->temperature;
            return 0;
        case PROP_SPI_MODE:
            return metalclaw_spi_mode_get(inst, BINDING_SPI, out);
        case PROP_POWER_MODE:
            out->v[0] = metalclaw_regmap_get(&state->map, 0, ICM4268X_REG_PWR_MGMT0);
            return 0;
        case PROP_ACCEL_CONFIG:
            out->v[0] = metalclaw_regmap_get(&state->map, 0, ICM4268X_REG_ACCEL_CONFIG0);
            return 0;
        case PROP_GYRO_CONFIG:
            out->v[0] = metalclaw_regmap_get(&state->map, 0, ICM4268X_REG_GYRO_CONFIG0);
            return 0;
        case PROP_BANK:
            out->v[0] = state->map.active_bank;
            return 0;
        case PROP_RESETS:
            out->v[0] = state->resets;
            return 0;
        default:
            return -EINVAL;
    }
}

static int icm4268x_set(struct metalclaw_instance *inst, size_t property, const struct metalclaw_value *in) {
    struct icm4268x_state *state = inst->state;

    switch (property) {
        case PROP_ACCEL:
        case PROP_GYRO: {
            int16_t *axes = property == PROP_ACCEL ? state->accel : state->gyro;
            for (size_t i = 0; i < 3; i++) {
                axes[i] = (int16_t)in->v[i];
            }
            break;
        }
        case PROP_TEMPERATURE:
            state->temperature = (int16_t)in->v[0];
            break;
        case PROP_SPI_MODE:
            return metalclaw_spi_mode_set(inst, BINDING_SPI, in);
        default:
            return -EINVAL;
    }
    publish_sample(inst);
    return 0;
}

static const struct metalclaw_model_ops icm4268x_ops = {
    .init = icm4268x_init,
    .deinit = icm4268x_deinit,
    .reset = icm4268x_reset,
    .get = icm4268x_get,
    .set = icm4268x_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .usb_rx = NULL,
};

#define ICM4268X_MODEL(symbol, name, who) \
    static const struct icm4268x_config symbol##_config = { .who_am_i = who }; \
    const struct metalclaw_model symbol = { \
        .abi_version = METALCLAW_ABI_VERSION, \
        .id = name, \
        .family = "icm4268x", \
        .config = &symbol##_config, \
        .properties = icm4268x_properties, \
        .property_count = sizeof(icm4268x_properties) / sizeof(icm4268x_properties[0]), \
        .regions = NULL, \
        .region_count = 0, \
        .faults = NULL, \
        .fault_count = 0, \
        .bindings = icm4268x_bindings, \
        .binding_count = sizeof(icm4268x_bindings) / sizeof(icm4268x_bindings[0]), \
        .state_size = sizeof(struct icm4268x_state), \
        .ops = &icm4268x_ops, \
    }

ICM4268X_MODEL(metalclaw_model_icm42688, "icm42688", ICM4268X_WHO_AM_I_ICM42688);
ICM4268X_MODEL(metalclaw_model_icm42686, "icm42686", ICM4268X_WHO_AM_I_ICM42686);
