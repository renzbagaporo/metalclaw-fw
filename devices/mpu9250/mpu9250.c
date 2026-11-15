// SPDX-License-Identifier: Apache-2.0 OR MIT
// InvenSense MPU-9250 accelerometer and gyroscope on SPI or I2C, from the
// Product Specification PS-MPU-9250A-01 rev 1.0 and the Register Map
// RM-MPU-9250A-00 rev 1.4. The AK8963 magnetometer inside the package sits
// behind the part's auxiliary I2C master and is not modelled.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "mpu9250.h"

// RM-MPU-9250A-00 section 3, Register Map.
#define REG_SMPLRT_DIV 0x19
#define REG_GYRO_CONFIG 0x1b
#define REG_ACCEL_CONFIG 0x1c
#define REG_INT_STATUS 0x3a
#define REG_ACCEL_XOUT_H 0x3b
#define REG_TEMP_OUT_H 0x41
#define REG_GYRO_XOUT_H 0x43
#define REG_EXT_SENS_DATA_23 0x60
#define REG_PWR_MGMT_1 0x6b
#define REG_FIFO_COUNTH 0x72
#define REG_FIFO_R_W 0x74
#define REG_WHO_AM_I 0x75
#define REG_LAST 0x7e

// RM 4.34: H_RESET restores every register and clears itself; SLEEP;
// PD_PTAT powers the temperature sensor down. RM 4.6 and 4.7: the full-scale
// selections in bits 4:3. RM 4.21: RAW_DATA_RDY_INT, cleared by a read.
#define PWR_MGMT_1_H_RESET 0x80u
#define PWR_MGMT_1_SLEEP 0x40u
#define PWR_MGMT_1_PD_PTAT 0x08u
#define FS_SEL_SHIFT 3
#define INT_STATUS_RAW_DATA_RDY 0x01u

// RM section 3: every register resets to 0x00 except PWR_MGMT_1 (0x01,
// CLKSEL auto, awake) and WHO_AM_I (0x71).
#define PWR_MGMT_1_RESET 0x01u
#define WHO_AM_I_MPU9250 0x71u

// RM 4.4: with the DLPF in use the internal sample rate is 1 kHz, divided
// by 1 + SMPLRT_DIV.
#define INTERNAL_RATE_HZ 1000u

// PS 3.4.2: 333.87 LSB/degC, 0 LSB at 21 degC (RM 4.23's
// TEMP_degC = TEMP_OUT / Temp_Sensitivity + 21). In hundredths of an LSB.
#define TEMP_SENSITIVITY_CENTI 33387
#define TEMP_ROOM_MC 21000

enum {
    PROP_ACCELERATION,
    PROP_ANGULAR_RATE,
    PROP_TEMPERATURE,
    PROP_SPI_MODE,
};

enum {
    BINDING_I2C,
    BINDING_SPI,
};

struct mpu9250_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    struct mclw_spi_target *spi;
    int32_t accel_mg[3];
    int32_t gyro_mdps[3];
    int32_t temperature_mc;
    uint64_t next_sample_us;
};

// PS 7.5: the first byte's bit 7 is R/W (1 reads), bits 6:0 the register;
// a burst runs on through consecutive registers with no dummy byte.
static const struct mclw_regmap_framing mpu9250_framing = {
    .read_bit_mask = 0x80,
    .address_mask = 0x7f,
    .read_dummy_bytes = 0,
    .auto_increment = true,
};

static const struct mclw_property_desc mpu9250_properties[] = {
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
    MCLW_PROPERTY_SPI_MODE,
};

// One of the two buses must be bound. PS 7.5: data is latched on the rising
// edge of SCLK and changes on the falling edge, which SPI modes 0 and 3 both
// do; PS 3.6 gives 1 MHz for every register, 3.6.1 20 MHz for the sensor
// and interrupt registers.
static const struct mclw_binding_desc mpu9250_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
    {
        .name = "spi",
        .kind = MCLW_BINDING_SPI_TARGET,
        .required = false,
        .usb = NULL,
        .spi = {
            .modes = MCLW_SPI_MODE_BIT(0) | MCLW_SPI_MODE_BIT(3),
            .default_mode = 0,
            .msb_first = true,
        },
        .uart = { 0 },
    },
};

static int16_t MCLW_RAM_FUNC(to_counts)(int64_t numerator, int64_t denominator) {
    int64_t v = numerator >= 0 ? (numerator + denominator / 2) / denominator
                               : -((-numerator + denominator / 2) / denominator);
    return (int16_t)(v < INT16_MIN ? INT16_MIN : (v > INT16_MAX ? INT16_MAX : v));
}

// PS table 2: 16384 LSB/g at +-2 g, halving per ACCEL_FS_SEL step. PS
// table 1: 131 LSB/(deg/s) at +-250 deg/s, halving per GYRO_FS_SEL step.
static void MCLW_RAM_FUNC(publish)(struct mpu9250_state *s) {
    struct mclw_regmap *map = &s->map;
    uint8_t power = mclw_regmap_get(map, 0, REG_PWR_MGMT_1);
    unsigned afs = (mclw_regmap_get(map, 0, REG_ACCEL_CONFIG) >> FS_SEL_SHIFT) & 3u;
    unsigned fs = (mclw_regmap_get(map, 0, REG_GYRO_CONFIG) >> FS_SEL_SHIFT) & 3u;

    if (power & PWR_MGMT_1_SLEEP) {
        return;
    }
    for (uint8_t axis = 0; axis < 3; axis++) {
        int16_t a = to_counts((int64_t)s->accel_mg[axis] * (16384 >> afs), 1000);
        int16_t g = to_counts((int64_t)s->gyro_mdps[axis] * 131, (int64_t)1000 << fs);
        mclw_regmap_set_be16(map, 0, (uint8_t)(REG_ACCEL_XOUT_H + 2 * axis), (uint16_t)a);
        mclw_regmap_set_be16(map, 0, (uint8_t)(REG_GYRO_XOUT_H + 2 * axis), (uint16_t)g);
    }
    if ((power & PWR_MGMT_1_PD_PTAT) == 0) {
        int16_t t = to_counts(((int64_t)s->temperature_mc - TEMP_ROOM_MC) * TEMP_SENSITIVITY_CENTI, 100000);
        mclw_regmap_set_be16(map, 0, REG_TEMP_OUT_H, (uint16_t)t);
    }
    mclw_regmap_set(map, 0, REG_INT_STATUS, INT_STATUS_RAW_DATA_RDY, MCLW_REG_CLEAR_ON_READ);
}

// The registers RM section 3 lists R/W are writable; the results, the
// status registers and WHO_AM_I are not. Addresses it does not list read 0.
static void MCLW_RAM_FUNC(reset_registers)(struct mpu9250_state *s) {
    struct mclw_regmap *map = &s->map;

    for (unsigned reg = 0; reg <= REG_LAST; reg++) {
        bool read_only = (reg >= 0x03 && reg <= 0x0c) || (reg >= 0x10 && reg <= 0x12) ||
            (reg >= 0x20 && reg <= 0x22) || (reg >= 0x35 && reg <= 0x36) || reg == 0x39 ||
            (reg >= REG_INT_STATUS && reg <= REG_EXT_SENS_DATA_23) || (reg >= 0x61 && reg <= 0x62) ||
            (reg >= 0x6d && reg <= 0x71) || reg == REG_WHO_AM_I || reg == 0x76 || reg == 0x79 ||
            reg == 0x7c;
        mclw_regmap_set(map, 0, (uint8_t)reg, 0, read_only ? 0 : MCLW_REG_WRITABLE);
    }
    mclw_regmap_set(map, 0, REG_INT_STATUS, 0, MCLW_REG_CLEAR_ON_READ);
    mclw_regmap_set(map, 0, REG_PWR_MGMT_1, PWR_MGMT_1_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, REG_WHO_AM_I, WHO_AM_I_MPU9250, 0);
    publish(s);
}

static void MCLW_RAM_FUNC(mpu9250_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx) {
    struct mpu9250_state *s = ctx;

    (void)map;
    (void)bank;
    if (reg == REG_PWR_MGMT_1 && (value & PWR_MGMT_1_H_RESET) != 0) {
        reset_registers(s);
    } else if (reg == REG_PWR_MGMT_1 || reg == REG_ACCEL_CONFIG || reg == REG_GYRO_CONFIG) {
        publish(s);
    }
}

static void defaults(struct mpu9250_state *s) {
    for (uint8_t axis = 0; axis < 3; axis++) {
        s->accel_mg[axis] = 0;
        s->gyro_mdps[axis] = 0;
    }
    s->accel_mg[2] = 1000;
    s->temperature_mc = 25000;
}

static int mpu9250_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct mpu9250_state *s = inst->state;

    if (bindings[BINDING_I2C].present == bindings[BINDING_SPI].present) {
        return -EINVAL;
    }
    s->i2c = bindings[BINDING_I2C].present ? bindings[BINDING_I2C].i2c : NULL;
    s->spi = bindings[BINDING_SPI].present ? bindings[BINDING_SPI].spi : NULL;
    defaults(s);
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    reset_registers(s);
    mclw_regmap_set_on_write(&s->map, mpu9250_on_write, s);
    if (s->i2c != NULL) {
        return mclw_hal_i2c_target_arm(s->i2c, &s->map);
    }
    return mclw_hal_spi_target_arm_regmap(s->spi, &s->map, &mpu9250_framing);
}

static void mpu9250_deinit(struct mclw_instance *inst) {
    struct mpu9250_state *s = inst->state;

    if (s->i2c != NULL) {
        mclw_hal_i2c_target_disarm(s->i2c);
    }
    if (s->spi != NULL) {
        mclw_hal_spi_target_disarm(s->spi);
    }
    s->i2c = NULL;
    s->spi = NULL;
}

static int mpu9250_reset(struct mclw_instance *inst, bool factory) {
    struct mpu9250_state *s = inst->state;

    (void)factory;
    uint32_t critical = mclw_hal_critical_enter();
    defaults(s);
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int mpu9250_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct mpu9250_state *s = inst->state;

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
        case PROP_SPI_MODE:
            return mclw_spi_mode_get(inst, BINDING_SPI, out);
        default:
            return -EINVAL;
    }
}

static int mpu9250_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct mpu9250_state *s = inst->state;

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
        case PROP_SPI_MODE:
            return mclw_spi_mode_set(inst, BINDING_SPI, in);
        default:
            return -EINVAL;
    }
    uint32_t critical = mclw_hal_critical_enter();
    publish(s);
    mclw_hal_critical_exit(critical);
    mclw_trace(inst, MCLW_TRACE_STATE, REG_ACCEL_XOUT_H, 14, 0);
    return 0;
}

// A new sample, and RAW_DATA_RDY_INT, every sample period while awake.
static void mpu9250_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct mpu9250_state *s = inst->state;

    if (now_us < s->next_sample_us) {
        return;
    }
    uint32_t critical = mclw_hal_critical_enter();
    uint32_t divider = (uint32_t)mclw_regmap_get(&s->map, 0, REG_SMPLRT_DIV) + 1u;
    publish(s);
    mclw_hal_critical_exit(critical);
    s->next_sample_us = now_us + (uint64_t)divider * (1000000u / INTERNAL_RATE_HZ);
}

static const struct mclw_model_ops mpu9250_ops = {
    .init = mpu9250_init,
    .deinit = mpu9250_deinit,
    .reset = mpu9250_reset,
    .get = mpu9250_get,
    .set = mpu9250_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = mpu9250_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_mpu9250 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "mpu9250",
    .family = "mpu9250",
    .config = NULL,
    .properties = mpu9250_properties,
    .property_count = sizeof(mpu9250_properties) / sizeof(mpu9250_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = mpu9250_bindings,
    .binding_count = sizeof(mpu9250_bindings) / sizeof(mpu9250_bindings[0]),
    .state_size = sizeof(struct mpu9250_state),
    .ops = &mpu9250_ops,
};
