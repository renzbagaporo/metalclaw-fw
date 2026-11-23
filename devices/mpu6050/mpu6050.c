// SPDX-License-Identifier: Apache-2.0 OR MIT
// InvenSense MPU-6050 six-axis IMU on I2C. Register addresses, bits and scales
// come from the InvenSense-authored Linux driver
// (drivers/iio/imu/inv_mpu6050/inv_mpu_iio.h and inv_mpu_core.c,
// torvalds/linux d23d763) and from the temperature formula pico-examples'
// mpu6050_i2c.c applies; see README.md.

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

#include "mpu6050.h"

// inv_mpu_iio.h
#define REG_SAMPLE_RATE_DIV 0x19
#define REG_CONFIG 0x1a
#define REG_GYRO_CONFIG 0x1b
#define REG_ACCEL_CONFIG 0x1c
#define REG_INT_PIN_CFG 0x37
#define REG_INT_ENABLE 0x38
#define REG_INT_STATUS 0x3a
#define REG_ACCEL_XOUT_H 0x3b
#define REG_TEMP_OUT_H 0x41
#define REG_GYRO_XOUT_H 0x43
#define REG_EXT_SENS_DATA_23 0x60
#define REG_PWR_MGMT_1 0x6b
#define REG_WHO_AM_I 0x75

#define PWR_MGMT_1_H_RESET 0x80u
#define PWR_MGMT_1_SLEEP 0x40u
#define PWR_MGMT_1_TEMP_DIS 0x08u
#define INT_STATUS_DATA_RDY 0x01u
// INT_PIN_CFG and INT_ENABLE (register map RM-MPU-6000A 4.15, 4.16).
#define INT_PIN_CFG_LEVEL_LOW 0x80u
#define INT_PIN_CFG_LATCH 0x20u
#define INT_ENABLE_DATA_RDY 0x01u
// Unlatched, INT emits a 50 us pulse per data-ready (4.15).
#define INT_PULSE_US 50u
#define CONFIG_FSR_SHIFT 3
#define WHO_AM_I_MPU6050 0x68

// The driver leaves PWR_MGMT_1 asleep until it clears SLEEP ("After reset,
// the sleep bit could be on", inv_mpu_core.c); the part powers up asleep.
#define PWR_MGMT_1_RESET PWR_MGMT_1_SLEEP

// The driver's sample clock is 1 kHz divided by SMPLRT_DIV + 1
// (INV_MPU6050_INTERNAL_FREQ_HZ, INV_MPU6050_FREQ_DIVIDER).
#define INTERNAL_RATE_HZ 1000u

enum {
    PROP_ACCELERATION,
    PROP_ANGULAR_RATE,
    PROP_TEMPERATURE,
};

enum {
    BINDING_I2C,
    BINDING_INT,
};

struct mpu6050_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    int32_t accel_mg[3];
    int32_t gyro_mdps[3];
    int32_t temperature_mc;
    uint64_t next_sample_us;
    // The INT pin, when bound.
    struct mclw_gpio *int_pin;
};

static const struct mclw_property_desc mpu6050_properties[] = {
    {
        .name = "acceleration",
        .type = MCLW_TYPE_I32,
        .count = 3,
        .access = MCLW_ACCESS_RW,
        .unit = "mg",
        .min = -16000,
        .max = 16000,
    },
    {
        .name = "angular_rate",
        .type = MCLW_TYPE_I32,
        .count = 3,
        .access = MCLW_ACCESS_RW,
        .unit = "mdps",
        .min = -2000000,
        .max = 2000000,
    },
    {
        .name = "temperature",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mC",
        .min = -40000,
        .max = 85000,
    },
};

static const struct mclw_binding_desc mpu6050_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
    {
        .name = "int",
        .kind = MCLW_BINDING_GPIO_OUT,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static int16_t MCLW_RAM_FUNC(to_counts)(int64_t numerator, int64_t denominator) {
    int64_t v = numerator >= 0 ? (numerator + denominator / 2) / denominator
                               : -((-numerator + denominator / 2) / denominator);
    return (int16_t)(v < INT16_MIN ? INT16_MIN : (v > INT16_MAX ? INT16_MAX : v));
}

// Accelerometer 16384 LSB/g at +-2 g, halving per AFS_SEL step (the
// driver's accel_scale: 598, 1196, 2392, 4785 um/s^2 per LSB). Gyroscope
// 131 LSB/(deg/s) at +-250 deg/s, halving per FS_SEL step (gyro_scale_6050
// doubles per step). Temperature: raw / 340 + 36.53 C (mpu6050_i2c.c; the
// driver's INV_MPU6050_TEMP_OFFSET 12420 and TEMP_SCALE 2941176 nC).
static void MCLW_RAM_FUNC(publish)(struct mpu6050_state *s) {
    struct mclw_regmap *map = &s->map;
    uint8_t power = mclw_regmap_get(map, 0, REG_PWR_MGMT_1);
    unsigned afs = (mclw_regmap_get(map, 0, REG_ACCEL_CONFIG) >> CONFIG_FSR_SHIFT) & 3u;
    unsigned fs = (mclw_regmap_get(map, 0, REG_GYRO_CONFIG) >> CONFIG_FSR_SHIFT) & 3u;

    if (power & PWR_MGMT_1_SLEEP) {
        return;
    }
    for (uint8_t axis = 0; axis < 3; axis++) {
        int16_t a = to_counts((int64_t)s->accel_mg[axis] * (16384 >> afs), 1000);
        int16_t g = to_counts((int64_t)s->gyro_mdps[axis] * 131, (int64_t)1000 << fs);
        mclw_regmap_set_be16(map, 0, (uint8_t)(REG_ACCEL_XOUT_H + 2 * axis), (uint16_t)a);
        mclw_regmap_set_be16(map, 0, (uint8_t)(REG_GYRO_XOUT_H + 2 * axis), (uint16_t)g);
    }
    if ((power & PWR_MGMT_1_TEMP_DIS) == 0) {
        int16_t t = to_counts(((int64_t)s->temperature_mc - 36530) * 340, 1000);
        mclw_regmap_set_be16(map, 0, REG_TEMP_OUT_H, (uint16_t)t);
    }
    mclw_regmap_set(map, 0, REG_INT_STATUS, INT_STATUS_DATA_RDY, MCLW_REG_CLEAR_ON_READ);
}

// Every register zero and writable except the results, INT_STATUS and
// WHO_AM_I, which the bus cannot write, and PWR_MGMT_1.
static void MCLW_RAM_FUNC(reset_registers)(struct mpu6050_state *s) {
    struct mclw_regmap *map = &s->map;

    for (unsigned reg = 0; reg <= REG_WHO_AM_I; reg++) {
        bool result = reg >= REG_INT_STATUS && reg <= REG_EXT_SENS_DATA_23;
        mclw_regmap_set(map, 0, (uint8_t)reg, 0, result ? 0 : MCLW_REG_WRITABLE);
    }
    mclw_regmap_set(map, 0, REG_INT_STATUS, 0, MCLW_REG_CLEAR_ON_READ);
    mclw_regmap_set(map, 0, REG_PWR_MGMT_1, PWR_MGMT_1_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, REG_WHO_AM_I, WHO_AM_I_MPU6050, 0);
}

static void MCLW_RAM_FUNC(mpu6050_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx) {
    struct mpu6050_state *s = ctx;

    (void)map;
    (void)bank;
    if (reg == REG_PWR_MGMT_1 && (value & PWR_MGMT_1_H_RESET) != 0) {
        reset_registers(s);
    } else if (reg == REG_PWR_MGMT_1 || reg == REG_ACCEL_CONFIG || reg == REG_GYRO_CONFIG) {
        publish(s);
    }
}

static void build_map(struct mpu6050_state *s) {
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    reset_registers(s);
    mclw_regmap_set_on_write(&s->map, mpu6050_on_write, s);
}

static void defaults(struct mpu6050_state *s) {
    for (uint8_t axis = 0; axis < 3; axis++) {
        s->accel_mg[axis] = 0;
        s->gyro_mdps[axis] = 0;
    }
    s->accel_mg[2] = 1000;
    s->temperature_mc = 25000;
}

static int mpu6050_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct mpu6050_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->int_pin = bindings[BINDING_INT].present ? bindings[BINDING_INT].gpio : NULL;
    defaults(s);
    build_map(s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void mpu6050_deinit(struct mclw_instance *inst) {
    struct mpu6050_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
    s->int_pin = NULL;
}

static int mpu6050_reset(struct mclw_instance *inst, bool factory) {
    struct mpu6050_state *s = inst->state;

    (void)factory;
    uint32_t critical = mclw_hal_critical_enter();
    defaults(s);
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int mpu6050_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct mpu6050_state *s = inst->state;

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
        default:
            return -EINVAL;
    }
}

static int mpu6050_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct mpu6050_state *s = inst->state;

    switch (property) {
        case PROP_ACCELERATION:
            for (uint8_t axis = 0; axis < 3; axis++) {
                s->accel_mg[axis] = (int32_t)in->v[axis];
            }
            break;
        case PROP_ANGULAR_RATE:
            for (uint8_t axis = 0; axis < 3; axis++) {
                s->gyro_mdps[axis] = (int32_t)in->v[axis];
            }
            break;
        case PROP_TEMPERATURE:
            s->temperature_mc = (int32_t)in->v[0];
            break;
        default:
            return -EINVAL;
    }
    uint32_t critical = mclw_hal_critical_enter();
    publish(s);
    mclw_hal_critical_exit(critical);
    mclw_trace(inst, MCLW_TRACE_STATE, REG_ACCEL_XOUT_H, 14, 0);
    return 0;
}

// A new sample, and DATA_RDY, every sample period while awake.
// The INT pin (4.15, 4.16): while DATA_RDY_EN is set, each data ready
// emits a 50 us pulse, or with LATCH_INT_EN holds INT until INT_STATUS
// clears; INT_LEVEL sets the active level. Push-pull either way (INT_OPEN is
// not modelled).
static bool int_enabled(struct mpu6050_state *s) {
    return (mclw_regmap_get(&s->map, 0, REG_INT_ENABLE) & INT_ENABLE_DATA_RDY) != 0;
}

static void write_int(struct mpu6050_state *s, bool active) {
    bool low = (mclw_regmap_get(&s->map, 0, REG_INT_PIN_CFG) & INT_PIN_CFG_LEVEL_LOW) != 0;

    mclw_hal_gpio_write(s->int_pin, active != low);
}

static void drive_int(struct mpu6050_state *s, bool sampled) {
    bool latched = (mclw_regmap_get(&s->map, 0, REG_INT_PIN_CFG) & INT_PIN_CFG_LATCH) != 0;
    bool ready = (mclw_regmap_get(&s->map, 0, REG_INT_STATUS) & INT_STATUS_DATA_RDY) != 0;

    if (s->int_pin == NULL) {
        return;
    }
    if (sampled && !latched && int_enabled(s)) {
        write_int(s, true);
        mclw_hal_delay_us(INT_PULSE_US);
    }
    write_int(s, latched && ready && int_enabled(s));
}

static void mpu6050_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct mpu6050_state *s = inst->state;

    if (now_us >= s->next_sample_us) {
        uint32_t critical = mclw_hal_critical_enter();
        uint32_t divider = (uint32_t)mclw_regmap_get(&s->map, 0, REG_SAMPLE_RATE_DIV) + 1u;
        publish(s);
        mclw_hal_critical_exit(critical);
        s->next_sample_us = now_us + (uint64_t)divider * (1000000u / INTERNAL_RATE_HZ);
        drive_int(s, true);
        return;
    }
    drive_int(s, false);
}

static const struct mclw_model_ops mpu6050_ops = {
    .init = mpu6050_init,
    .deinit = mpu6050_deinit,
    .reset = mpu6050_reset,
    .get = mpu6050_get,
    .set = mpu6050_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = mpu6050_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_mpu6050 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "mpu6050",
    .family = "mpu6050",
    .config = NULL,
    .properties = mpu6050_properties,
    .property_count = sizeof(mpu6050_properties) / sizeof(mpu6050_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = mpu6050_bindings,
    .binding_count = sizeof(mpu6050_bindings) / sizeof(mpu6050_bindings[0]),
    .state_size = sizeof(struct mpu6050_state),
    .ops = &mpu6050_ops,
};
