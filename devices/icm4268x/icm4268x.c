// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/bus.h"
#include "metalclaw/hal/gpio.h"
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
    BINDING_INT1,
};

struct icm4268x_config {
    uint8_t who_am_i;
};

struct icm4268x_state {
    struct mclw_regmap map;
    struct mclw_spi_target *spi;
    uint8_t who_am_i;
    int16_t accel[3];
    int16_t gyro[3];
    int16_t temperature;
    uint32_t resets;
    // The INT1 pin, when bound, and when the next sample is due.
    struct mclw_gpio *int1;
    uint64_t next_sample_us;
};

static const struct mclw_property_desc icm4268x_properties[] = {
    {
        .name = "accel",
        .type = MCLW_TYPE_I32,
        .count = 3,
        .access = MCLW_ACCESS_RW,
        .unit = "LSB",
        .min = INT16_MIN,
        .max = INT16_MAX,
    },
    {
        .name = "gyro",
        .type = MCLW_TYPE_I32,
        .count = 3,
        .access = MCLW_ACCESS_RW,
        .unit = "LSB",
        .min = INT16_MIN,
        .max = INT16_MAX,
    },
    {
        .name = "temperature",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "LSB",
        .min = INT16_MIN,
        .max = INT16_MAX,
    },
    MCLW_PROPERTY_SPI_MODE,
    {
        .name = "power_mode",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 0xff,
    },
    {
        .name = "accel_config",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 0xff,
    },
    {
        .name = "gyro_config",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 0xff,
    },
    {
        .name = "bank",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = ICM4268X_BANKS - 1,
    },
    {
        .name = "resets",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
};

static const struct mclw_binding_desc icm4268x_bindings[] = {
    {
        .name = "spi",
        .kind = MCLW_BINDING_SPI_TARGET,
        .required = true,
        .usb = NULL,
        .spi = {
            .modes = MCLW_SPI_MODE_BIT(0) | MCLW_SPI_MODE_BIT(3),
            .default_mode = 3,
            .msb_first = true,
        },
        .uart = { 0 },
    },
    {
        .name = "int1",
        .kind = MCLW_BINDING_GPIO_OUT,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static const struct mclw_regmap_framing icm4268x_framing = {
    .read_bit_mask = ICM4268X_SPI_READ_BIT,
    .address_mask = ICM4268X_SPI_ADDRESS_MASK,
    .read_dummy_bytes = 0,
    .auto_increment = true,
};

static void MCLW_RAM_FUNC(load_sample)(struct icm4268x_state *state) {
    mclw_regmap_set_be16(&state->map, 0, ICM4268X_REG_TEMP_DATA1, (uint16_t)state->temperature);
    for (uint8_t i = 0; i < 3; i++) {
        mclw_regmap_set_be16(&state->map, 0, (uint8_t)(ICM4268X_REG_ACCEL_DATA_X1 + 2 * i), (uint16_t)state->accel[i]);
        mclw_regmap_set_be16(&state->map, 0, (uint8_t)(ICM4268X_REG_GYRO_DATA_X1 + 2 * i), (uint16_t)state->gyro[i]);
    }
}

// Power-on state, as the part leaves it after a soft reset: every register
// writable and zero except the few with a documented reset value.
static void MCLW_RAM_FUNC(build_map)(struct icm4268x_state *state) {
    struct mclw_regmap *map = &state->map;

    for (uint8_t bank = 0; bank < ICM4268X_BANKS; bank++) {
        memset(map->value[bank], 0, MCLW_REGMAP_BANK_SIZE);
        memset(map->flags[bank], MCLW_REG_WRITABLE, MCLW_REGMAP_BANK_SIZE);
    }
    map->active_bank = 0;
    mclw_regmap_set(map, 0, ICM4268X_REG_WHO_AM_I, state->who_am_i, 0);
    // INT_STATUS clears when read (14.21).
    mclw_regmap_set(map, 0, ICM4268X_REG_INT_STATUS, ICM4268X_INT_STATUS_RESET_DONE, MCLW_REG_CLEAR_ON_READ);
    mclw_regmap_set(map, 0, ICM4268X_REG_INT_SOURCE0, ICM4268X_INT_SOURCE0_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, ICM4268X_REG_TMST_CONFIG, ICM4268X_TMST_CONFIG_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, ICM4268X_REG_INTF_CONFIG1, ICM4268X_INTF_CONFIG1_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, ICM4268X_REG_INT_CONFIG1, ICM4268X_INT_CONFIG1_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, ICM4268X_REG_ACCEL_CONFIG0, ICM4268X_ACCEL_CONFIG0_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, ICM4268X_REG_GYRO_CONFIG0, ICM4268X_GYRO_CONFIG0_RESET, MCLW_REG_WRITABLE);
    for (uint8_t reg = ICM4268X_REG_TEMP_DATA1; reg < ICM4268X_REG_TEMP_DATA1 + ICM4268X_SAMPLE_BYTES; reg++) {
        mclw_regmap_set(map, 0, reg, 0, 0);
    }
    load_sample(state);
}

static void MCLW_RAM_FUNC(raise_data_ready)(struct icm4268x_state *state) {
    struct mclw_regmap *map = &state->map;
    if (mclw_regmap_get(map, 0, ICM4268X_REG_PWR_MGMT0) & ICM4268X_PWR_MGMT0_SENSOR_MASK) {
        mclw_regmap_set(map, 0, ICM4268X_REG_INT_STATUS,
            mclw_regmap_get(map, 0, ICM4268X_REG_INT_STATUS) | ICM4268X_INT_STATUS_DATA_RDY,
            MCLW_REG_CLEAR_ON_READ);
    }
}

// Interrupt context, after the engine committed a bus write and before it
// re-arms, so it and everything it calls are in RAM.
static void MCLW_RAM_FUNC(on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value, void *ctx) {
    struct icm4268x_state *state = ctx;
    (void)map;

    if (bank != 0) {
        return;
    }
    if (reg == ICM4268X_REG_DEVICE_CONFIG && (value & ICM4268X_DEVICE_CONFIG_SOFT_RESET)) {
        build_map(state);
        state->resets++;
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_COMMAND, reg, 1, 0);
    } else if (reg == ICM4268X_REG_PWR_MGMT0) {
        raise_data_ready(state);
    }
}

static int icm4268x_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct icm4268x_state *state = inst->state;

    state->spi = bindings[BINDING_SPI].spi;
    state->int1 = bindings[BINDING_INT1].present ? bindings[BINDING_INT1].gpio : NULL;
    state->next_sample_us = 0;
    state->who_am_i = ((const struct icm4268x_config *)inst->model->config)->who_am_i;
    mclw_regmap_init(&state->map, ICM4268X_BANKS, ICM4268X_REG_BANK_SEL, 0x00);
    mclw_regmap_set_on_write(&state->map, on_write, state);
    build_map(state);
    return mclw_hal_spi_target_arm_regmap(state->spi, &state->map, &icm4268x_framing);
}

static void icm4268x_deinit(struct mclw_instance *inst) {
    struct icm4268x_state *state = inst->state;

    mclw_hal_spi_target_disarm(state->spi);
    state->spi = NULL;
    state->int1 = NULL;
}

static int icm4268x_reset(struct mclw_instance *inst, bool factory) {
    struct icm4268x_state *state = inst->state;
    uint32_t critical;

    (void)factory;
    memset(state->accel, 0, sizeof(state->accel));
    memset(state->gyro, 0, sizeof(state->gyro));
    state->temperature = 0;
    critical = mclw_hal_critical_enter();
    build_map(state);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int icm4268x_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
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
            return mclw_spi_mode_get(inst, BINDING_SPI, out);
        case PROP_POWER_MODE:
            out->v[0] = mclw_regmap_get(&state->map, 0, ICM4268X_REG_PWR_MGMT0);
            return 0;
        case PROP_ACCEL_CONFIG:
            out->v[0] = mclw_regmap_get(&state->map, 0, ICM4268X_REG_ACCEL_CONFIG0);
            return 0;
        case PROP_GYRO_CONFIG:
            out->v[0] = mclw_regmap_get(&state->map, 0, ICM4268X_REG_GYRO_CONFIG0);
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

static int icm4268x_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct icm4268x_state *state = inst->state;
    uint32_t critical;

    switch (property) {
        case PROP_ACCEL:
        case PROP_GYRO:
        case PROP_TEMPERATURE:
            break;
        case PROP_SPI_MODE:
            return mclw_spi_mode_set(inst, BINDING_SPI, in);
        default:
            return -EINVAL;
    }
    // The state and the image change together: a soft reset in on_write
    // rebuilds the image from the state, so it must not run between them.
    critical = mclw_hal_critical_enter();
    if (property == PROP_TEMPERATURE) {
        state->temperature = (int16_t)in->v[0];
    } else {
        int16_t *axes = property == PROP_ACCEL ? state->accel : state->gyro;

        for (size_t i = 0; i < 3; i++) {
            axes[i] = (int16_t)in->v[i];
        }
    }
    load_sample(state);
    raise_data_ready(state);
    mclw_hal_critical_exit(critical);
    mclw_trace(inst, MCLW_TRACE_STATE, ICM4268X_REG_TEMP_DATA1, ICM4268X_SAMPLE_BYTES, 0);
    return 0;
}

// The sample period of an ACCEL_CONFIG0 or GYRO_CONFIG0 ODR code (14.37,
// 14.38), in microseconds; 0 for the reserved code.
static uint32_t odr_period_us(uint8_t code) {
    static const uint32_t period_us[16] = {
        0, 31, 63, 125, 250, 500, 1000, 5000, 10000, 20000, 40000, 80000, 160000, 320000, 640000, 2000,
    };

    return period_us[code & ICM4268X_CONFIG0_ODR_MASK];
}

// INT1 at `level`: push-pull drives it either way; open drain (14.3,
// INT1_DRIVE_CIRCUIT 0, the reset value) only pulls it low and otherwise
// lets go.
static void write_int1(struct icm4268x_state *state, bool level) {
    if (level && (mclw_regmap_get(&state->map, 0, ICM4268X_REG_INT_CONFIG) & ICM4268X_INT_CONFIG_INT1_PUSH_PULL) == 0) {
        mclw_hal_gpio_release(state->int1);
    } else {
        mclw_hal_gpio_write(state->int1, level);
    }
}

// Drives INT1 (14.3, 14.50, 14.51): with UI_DRDY_INT1_EN, each sample's
// data ready emits a 100 us pulse (8 us with INT_TPULSE_DURATION), or in
// latched mode holds INT1 until INT_STATUS is read; INT1_POLARITY sets the
// active level.
static void drive_int1(struct icm4268x_state *state, bool sampled) {
    struct mclw_regmap *map = &state->map;
    uint8_t config = mclw_regmap_get(map, 0, ICM4268X_REG_INT_CONFIG);
    bool high = (config & ICM4268X_INT_CONFIG_INT1_POLARITY_HIGH) != 0;
    bool latched = (config & ICM4268X_INT_CONFIG_INT1_MODE_LATCHED) != 0;
    bool routed = (mclw_regmap_get(map, 0, ICM4268X_REG_INT_SOURCE0) & ICM4268X_INT_SOURCE0_UI_DRDY_INT1_EN) != 0;
    bool ready = (mclw_regmap_get(map, 0, ICM4268X_REG_INT_STATUS) & ICM4268X_INT_STATUS_DATA_RDY) != 0;

    if (state->int1 == NULL) {
        return;
    }
    if (sampled && routed && !latched) {
        bool short_pulse = (mclw_regmap_get(map, 0, ICM4268X_REG_INT_CONFIG1) & ICM4268X_INT_CONFIG1_TPULSE_8US) != 0;

        write_int1(state, high);
        mclw_hal_delay_us(short_pulse ? 8u : 100u);
    }
    write_int1(state, (routed && latched && ready) == high);
}

// A new sample at the ODR of the accelerometer, or the gyroscope when only
// it runs, once per tick at most: the simulation clock ticks every
// millisecond, so ODRs above 1 kHz deliver at 1 kHz.
static void icm4268x_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct icm4268x_state *state = inst->state;
    struct mclw_regmap *map = &state->map;
    uint8_t power = mclw_regmap_get(map, 0, ICM4268X_REG_PWR_MGMT0);
    uint8_t code = (power & ICM4268X_PWR_MGMT0_ACCEL_MODE_MASK) != 0
        ? mclw_regmap_get(map, 0, ICM4268X_REG_ACCEL_CONFIG0)
        : mclw_regmap_get(map, 0, ICM4268X_REG_GYRO_CONFIG0);
    uint32_t period_us = odr_period_us(code);
    bool sampled = false;

    if ((power & ICM4268X_PWR_MGMT0_SENSOR_MASK) != 0 && period_us != 0 && now_us >= state->next_sample_us) {
        uint32_t critical = mclw_hal_critical_enter();

        load_sample(state);
        raise_data_ready(state);
        mclw_hal_critical_exit(critical);
        state->next_sample_us = now_us + period_us;
        sampled = true;
    }
    drive_int1(state, sampled);
}

static const struct mclw_model_ops icm4268x_ops = {
    .init = icm4268x_init,
    .deinit = icm4268x_deinit,
    .reset = icm4268x_reset,
    .get = icm4268x_get,
    .set = icm4268x_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = icm4268x_time_advanced,
    .usb_rx = NULL,
};

#define ICM4268X_MODEL(symbol, name, who) \
    static const struct icm4268x_config symbol##_config = { .who_am_i = who }; \
    const struct mclw_model symbol = { \
        .abi_version = MCLW_ABI_VERSION, \
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

ICM4268X_MODEL(mclw_model_icm42688, "icm42688", ICM4268X_WHO_AM_I_ICM42688);
ICM4268X_MODEL(mclw_model_icm42686, "icm42686", ICM4268X_WHO_AM_I_ICM42686);
