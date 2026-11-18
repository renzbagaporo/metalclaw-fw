// SPDX-License-Identifier: Apache-2.0 OR MIT
// ST LSM6DSO 6-axis IMU on I2C: the main register page as a register map
// the harness serves from hardware, sampling the accelerometer and
// gyroscope at their ODRs on the tick, with data-ready on the INT1 pin.
// Table numbers are the datasheet's, DS12140 Rev 3 (June 2024).

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

// Table 20, registers address map (main page).
#define REG_FUNC_CFG_ACCESS 0x01u
#define REG_PIN_CTRL 0x02u
#define REG_COUNTER_BDR_REG1 0x0bu
#define REG_INT1_CTRL 0x0du
#define REG_WHO_AM_I 0x0fu
#define REG_CTRL1_XL 0x10u
#define REG_CTRL2_G 0x11u
#define REG_CTRL3_C 0x12u
#define REG_CTRL9_XL 0x18u
#define REG_ALL_INT_SRC 0x1au
#define REG_STATUS 0x1eu
#define REG_OUT_TEMP_L 0x20u
#define REG_OUTX_L_G 0x22u
#define REG_OUTX_L_A 0x28u
#define REG_LAST 0x7fu
#define WHO_AM_I_LSM6DSO 0x6cu
// CTRL3_C (table 52): BOOT 7, H_LACTIVE 5 (1 active low), SW_RESET 0;
// reset value 04h (IF_INC). COUNTER_BDR_REG1's dataready_pulsed is bit 7.
// INT1_CTRL: INT1_DRDY_XL 0, INT1_DRDY_G 1. STATUS_REG: XLDA 0, GDA 1, TDA 2.
#define CTRL3_BOOT 0x80u
#define CTRL3_H_LACTIVE 0x20u
#define CTRL3_SW_RESET 0x01u
#define BDR1_DATAREADY_PULSED 0x80u
#define STATUS_XLDA 0x01u
#define STATUS_GDA 0x02u
#define STATUS_TDA 0x04u
// Pulsed data-ready lasts 75 us (9.11).
#define DRDY_PULSE_US 75u

enum {
    PROP_ACCELERATION,
    PROP_ANGULAR_RATE,
    PROP_TEMPERATURE,
    PROP_SAMPLES,
};

enum {
    BINDING_I2C,
    BINDING_INT1,
};

struct lsm6dso_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    struct mclw_gpio *int1;
    int32_t accel_mg[3];
    int32_t gyro_mdps[3];
    int32_t temperature_mc;
    uint32_t samples;
    uint64_t next_xl_us;
    uint64_t next_g_us;
};

static const struct mclw_property_desc lsm6dso_properties[] = {
    { .name = "acceleration", .type = MCLW_TYPE_I32, .count = 3, .access = MCLW_ACCESS_RW, .unit = "mg",
      .min = -16000, .max = 16000 },
    { .name = "angular_rate", .type = MCLW_TYPE_I32, .count = 3, .access = MCLW_ACCESS_RW, .unit = "mdps",
      .min = -2000000, .max = 2000000 },
    { .name = "temperature", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mC",
      .min = -40000, .max = 85000 },
    { .name = "samples", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc lsm6dso_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
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

static uint8_t get(const struct lsm6dso_state *s, uint8_t reg) {
    return mclw_regmap_get(&s->map, 0, reg);
}

static int16_t MCLW_RAM_FUNC(to_counts)(int64_t numerator, int64_t denominator) {
    int64_t v = numerator >= 0 ? (numerator + denominator / 2) / denominator
                               : -((-numerator + denominator / 2) / denominator);
    return (int16_t)(v < INT16_MIN ? INT16_MIN : (v > INT16_MAX ? INT16_MAX : v));
}

// Table 20's reset values: PIN_CTRL 3Fh, WHO_AM_I 6Ch, CTRL3_C 04h,
// CTRL9_XL E0h, the rest zero; the sources, status and outputs are
// read-only, STATUS_REG clearing when read (see the README).
static void MCLW_RAM_FUNC(reset_registers)(struct lsm6dso_state *s) {
    for (uint16_t reg = 0; reg <= REG_LAST; reg++) {
        bool ro = reg == REG_WHO_AM_I || reg >= REG_ALL_INT_SRC;
        mclw_regmap_set(&s->map, 0, (uint8_t)reg, 0, ro ? 0 : MCLW_REG_WRITABLE);
    }
    mclw_regmap_set(&s->map, 0, REG_PIN_CTRL, 0x3f, MCLW_REG_WRITABLE);
    mclw_regmap_set(&s->map, 0, REG_WHO_AM_I, WHO_AM_I_LSM6DSO, 0);
    mclw_regmap_set(&s->map, 0, REG_CTRL3_C, 0x04, MCLW_REG_WRITABLE);
    mclw_regmap_set(&s->map, 0, REG_CTRL9_XL, 0xe0, MCLW_REG_WRITABLE);
    mclw_regmap_set(&s->map, 0, REG_STATUS, 0, MCLW_REG_CLEAR_ON_READ);
}

// SW_RESET and BOOT restore the registers and clear themselves (table 52).
static void MCLW_RAM_FUNC(lsm6dso_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct lsm6dso_state *s = ctx;

    (void)map;
    (void)bank;
    if (reg == REG_CTRL3_C && (value & (CTRL3_SW_RESET | CTRL3_BOOT)) != 0) {
        reset_registers(s);
    }
}

// Table 44: ODR 0001..1010 are 12.5 Hz to 6.66 kHz, doubling from 26 Hz;
// 1011 is the accelerometer's 1.6 Hz low-power rate. 0 is power-down.
static uint32_t odr_period_us(uint8_t code) {
    static const uint32_t period[16] = {
        0, 80000, 38462, 19231, 9615, 4808, 2404, 1200, 600, 300, 150, 625000, 0, 0, 0, 0,
    };

    return period[code & 0x0fu];
}

// Table 2: 0.061 mg/LSB at +-2 g, doubling with each FS step; FS_XL codes
// 00 2 g, 01 16 g, 10 4 g, 11 8 g (table 46).
static void MCLW_RAM_FUNC(sample_accel)(struct lsm6dso_state *s) {
    static const int64_t ug_per_lsb[4] = { 61, 488, 122, 244 };
    int64_t per = ug_per_lsb[(get(s, REG_CTRL1_XL) >> 2) & 3u];

    for (uint8_t axis = 0; axis < 3; axis++) {
        mclw_regmap_set_le16(&s->map, 0, (uint8_t)(REG_OUTX_L_A + 2u * axis),
            (uint16_t)to_counts((int64_t)s->accel_mg[axis] * 1000, per));
    }
}

// Table 2: 8.75 mdps/LSB at 250 dps, 17.5, 35 and 70 at 500, 1000 and 2000
// (FS_G 00-11, table 49), 4.375 at 125 dps with FS_125.
static void MCLW_RAM_FUNC(sample_gyro)(struct lsm6dso_state *s) {
    static const int64_t udps_per_lsb[4] = { 8750, 17500, 35000, 70000 };
    uint8_t ctrl2 = get(s, REG_CTRL2_G);
    int64_t per = (ctrl2 & 0x02u) != 0 ? 4375 : udps_per_lsb[(ctrl2 >> 2) & 3u];

    for (uint8_t axis = 0; axis < 3; axis++) {
        mclw_regmap_set_le16(&s->map, 0, (uint8_t)(REG_OUTX_L_G + 2u * axis),
            (uint16_t)to_counts((int64_t)s->gyro_mdps[axis] * 1000, per));
    }
}

// Table 3: 256 LSB/C, 0 at 25 C.
static void MCLW_RAM_FUNC(sample_temperature)(struct lsm6dso_state *s) {
    mclw_regmap_set_le16(&s->map, 0, REG_OUT_TEMP_L,
        (uint16_t)to_counts(((int64_t)s->temperature_mc - 25000) * 256, 1000));
}

// INT1 (9.11, tables 34-35): the routed data-ready flags, latched until
// read or, with dataready_pulsed, a 75 us pulse per sample; active high
// unless H_LACTIVE. Push-pull either way (PP_OD is not modelled).
static void drive_int1(struct lsm6dso_state *s, uint8_t new_flags) {
    uint8_t routed = get(s, REG_INT1_CTRL) & (STATUS_XLDA | STATUS_GDA);
    bool low = (get(s, REG_CTRL3_C) & CTRL3_H_LACTIVE) != 0;
    bool pulsed = (get(s, REG_COUNTER_BDR_REG1) & BDR1_DATAREADY_PULSED) != 0;

    if (s->int1 == NULL) {
        return;
    }
    if (pulsed) {
        if ((new_flags & routed) != 0) {
            mclw_hal_gpio_write(s->int1, !low);
            mclw_hal_delay_us(DRDY_PULSE_US);
        }
        mclw_hal_gpio_write(s->int1, low);
        return;
    }
    mclw_hal_gpio_write(s->int1, ((get(s, REG_STATUS) & routed) != 0) != low);
}

static int lsm6dso_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct lsm6dso_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->int1 = bindings[BINDING_INT1].present ? bindings[BINDING_INT1].gpio : NULL;
    for (uint8_t axis = 0; axis < 3; axis++) {
        s->accel_mg[axis] = 0;
        s->gyro_mdps[axis] = 0;
    }
    s->accel_mg[2] = 1000;
    s->temperature_mc = 25000;
    s->samples = 0;
    s->next_xl_us = 0;
    s->next_g_us = 0;
    // One page: FUNC_CFG_ACCESS's bank bits are not modelled (README).
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    mclw_regmap_set_on_write(&s->map, lsm6dso_on_write, s);
    uint32_t critical = mclw_hal_critical_enter();
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void lsm6dso_deinit(struct mclw_instance *inst) {
    struct lsm6dso_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
    s->int1 = NULL;
}

static int lsm6dso_reset(struct mclw_instance *inst, bool factory) {
    struct lsm6dso_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    s->samples = 0;
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int lsm6dso_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct lsm6dso_state *s = inst->state;

    switch (property) {
        case PROP_ACCELERATION:
            for (uint8_t axis = 0; axis < 3; axis++) {
                out->v[axis] = s->accel_mg[axis];
            }
            return 0;
        case PROP_ANGULAR_RATE:
            for (uint8_t axis = 0; axis < 3; axis++) {
                out->v[axis] = s->gyro_mdps[axis];
            }
            return 0;
        case PROP_TEMPERATURE:
            out->v[0] = s->temperature_mc;
            return 0;
        case PROP_SAMPLES:
            out->v[0] = s->samples;
            return 0;
        default:
            return -EINVAL;
    }
}

static int lsm6dso_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct lsm6dso_state *s = inst->state;

    switch (property) {
        case PROP_ACCELERATION:
            for (uint8_t axis = 0; axis < 3; axis++) {
                s->accel_mg[axis] = (int32_t)in->v[axis];
            }
            return 0;
        case PROP_ANGULAR_RATE:
            for (uint8_t axis = 0; axis < 3; axis++) {
                s->gyro_mdps[axis] = (int32_t)in->v[axis];
            }
            return 0;
        case PROP_TEMPERATURE:
            s->temperature_mc = (int32_t)in->v[0];
            return 0;
        default:
            return -EINVAL;
    }
}

// Each sensor samples at its own ODR, at most once a tick: the simulation
// clock ticks every millisecond, so ODRs above 1 kHz deliver at 1 kHz. A
// sample sets its STATUS_REG flag, the temperature with either.
static void lsm6dso_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct lsm6dso_state *s = inst->state;
    uint8_t flags = 0;
    uint32_t critical = mclw_hal_critical_enter();
    uint32_t xl = odr_period_us(get(s, REG_CTRL1_XL) >> 4);
    uint32_t g = odr_period_us(get(s, REG_CTRL2_G) >> 4);

    if (xl != 0 && now_us >= s->next_xl_us) {
        sample_accel(s);
        flags |= STATUS_XLDA;
        s->next_xl_us = now_us + xl;
    }
    if (g != 0 && g != 625000u && now_us >= s->next_g_us) {
        sample_gyro(s);
        flags |= STATUS_GDA;
        s->next_g_us = now_us + g;
    }
    if (flags != 0) {
        sample_temperature(s);
        mclw_regmap_set(&s->map, 0, REG_STATUS, (uint8_t)(get(s, REG_STATUS) | flags | STATUS_TDA),
            MCLW_REG_CLEAR_ON_READ);
        s->samples++;
    }
    mclw_hal_critical_exit(critical);
    drive_int1(s, flags);
}

static const struct mclw_model_ops lsm6dso_ops = {
    .init = lsm6dso_init,
    .deinit = lsm6dso_deinit,
    .reset = lsm6dso_reset,
    .get = lsm6dso_get,
    .set = lsm6dso_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = lsm6dso_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_lsm6dso = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "lsm6dso",
    .family = "lsm6dso",
    .config = NULL,
    .properties = lsm6dso_properties,
    .property_count = sizeof(lsm6dso_properties) / sizeof(lsm6dso_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = lsm6dso_bindings,
    .binding_count = sizeof(lsm6dso_bindings) / sizeof(lsm6dso_bindings[0]),
    .state_size = sizeof(struct lsm6dso_state),
    .ops = &lsm6dso_ops,
};
