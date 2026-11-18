// SPDX-License-Identifier: Apache-2.0 OR MIT
// InvenSense MPU-9250 accelerometer and gyroscope on SPI or I2C, from the
// Product Specification PS-MPU-9250A-01 rev 1.0 and the Register Map
// RM-MPU-9250A-00 rev 1.4. The AK8963 magnetometer inside the package sits
// behind the part's auxiliary I2C master: the model answers that master's
// slave 4 single transfers and slave 0 periodic reads with an AK8963 of
// its own, from the register map's section 5 and AKM's MS1356-E-02.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/gpio.h"
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
#define REG_I2C_SLV0_ADDR 0x25
#define REG_I2C_SLV0_REG 0x26
#define REG_I2C_SLV0_CTRL 0x27
#define REG_I2C_SLV4_ADDR 0x31
#define REG_I2C_SLV4_REG 0x32
#define REG_I2C_SLV4_DO 0x33
#define REG_I2C_SLV4_CTRL 0x34
#define REG_I2C_SLV4_DI 0x35
#define REG_I2C_MST_STATUS 0x36
#define REG_INT_PIN_CFG 0x37
#define REG_INT_ENABLE 0x38
#define REG_EXT_SENS_DATA_00 0x49
#define REG_USER_CTRL 0x6a
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
// RM 4.11-4.17, 4.20: a slave's address register has RNW in bit 7, its
// control register EN in bit 7 and (slave 0) LENG in bits 3:0; I2C_SLV4_EN
// clears when the transfer is done, I2C_MST_STATUS then has I2C_SLV4_DONE
// (bit 6) or I2C_SLV4_NACK (bit 4). USER_CTRL's I2C_MST_EN is bit 5.
#define SLV_RNW 0x80u
#define SLV_EN 0x80u
#define SLV0_LENG 0x0fu
#define MST_STATUS_SLV4_DONE 0x40u
#define MST_STATUS_SLV4_NACK 0x10u
#define USER_CTRL_I2C_MST_EN 0x20u
// INT_PIN_CFG (4.18): ACTL bit 7 (1 active low), LATCH_INT_EN bit 5;
// INT_ENABLE (4.19): RAW_RDY_EN bit 0. Unlatched, a 50 us pulse.
#define INT_PIN_CFG_ACTL 0x80u
#define INT_PIN_CFG_LATCH 0x20u
#define INT_ENABLE_RAW_RDY 0x01u
#define INT_PULSE_US 50u
// The AK8963 (register map section 5): at 0Ch, WIA 48h; ST1's DRDY bit 0
// and DOR bit 1; ST2's HOFL bit 3 and BITM bit 4; CNTL1's BIT bit 4 and
// MODE[3:0]; CNTL2's SRST; ASAX..ASAZ in the fuse ROM. 16 bits measure
// 0.15 uT a count, 14 bits 0.6 uT (MS1356-E-02, characteristics).
#define AK_ADDRESS 0x0cu
#define AK_REGISTERS 0x13u
#define AK_WIA 0x00u
#define AK_INFO 0x01u
#define AK_ST1 0x02u
#define AK_HXL 0x03u
#define AK_ST2 0x09u
#define AK_CNTL1 0x0au
#define AK_CNTL2 0x0bu
#define AK_ASAX 0x10u
#define AK_WIA_VALUE 0x48u
#define AK_ST1_DRDY 0x01u
#define AK_ST1_DOR 0x02u
#define AK_ST2_BITM 0x10u
#define AK_CNTL1_BIT 0x10u
#define AK_CNTL2_SRST 0x01u
#define AK_MODE_SINGLE 0x1u
#define AK_MODE_CONT1 0x2u
#define AK_MODE_CONT2 0x6u
// ASA 128: a sensitivity adjustment of exactly 1 (MS1356-E-02 8.3.11).
#define AK_ASA_VALUE 0x80u

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
    PROP_MAGNETIC_FIELD,
};

enum {
    BINDING_I2C,
    BINDING_SPI,
    BINDING_INT,
};

struct mpu9250_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    struct mclw_spi_target *spi;
    int32_t accel_mg[3];
    int32_t gyro_mdps[3];
    int32_t temperature_mc;
    uint64_t next_sample_us;
    // The AK8963: its registers, the field it measures, and its next
    // measurement in a continuous mode.
    uint8_t ak[AK_REGISTERS];
    int32_t magn_nt[3];
    uint64_t ak_next_us;
    // The INT pin, when bound.
    struct mclw_gpio *int_pin;
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
    {
        .name = "magnetic_field",
        .type = MCLW_TYPE_I32,
        .count = 3,
        .access = MCLW_ACCESS_RW,
        .unit = "nT",
        .min = -4900000,
        .max = 4900000,
    },
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
    {
        .name = "int",
        .kind = MCLW_BINDING_GPIO_OUT,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// The AK8963 after power-on or SRST (section 5): WIA, INFO, the fuse ROM
// and power-down; everything else zero.
static void MCLW_RAM_FUNC(ak_reset)(struct mpu9250_state *s) {
    for (uint8_t reg = 0; reg < AK_REGISTERS; reg++) {
        s->ak[reg] = 0;
    }
    s->ak[AK_WIA] = AK_WIA_VALUE;
    s->ak[AK_INFO] = 0x9a;
    s->ak[AK_ASAX] = AK_ASA_VALUE;
    s->ak[AK_ASAX + 1u] = AK_ASA_VALUE;
    s->ak[AK_ASAX + 2u] = AK_ASA_VALUE;
}

// A register read through the auxiliary bus. Reading ST2 ends a data read:
// DRDY and DOR clear (MS1356-E-02 6.4.3.2).
static uint8_t MCLW_RAM_FUNC(ak_read)(struct mpu9250_state *s, uint8_t reg) {
    uint8_t v = reg < AK_REGISTERS ? s->ak[reg] : 0;

    if (reg == AK_ST2) {
        s->ak[AK_ST1] = (uint8_t)(s->ak[AK_ST1] & ~(AK_ST1_DRDY | AK_ST1_DOR));
    }
    return v;
}

// CNTL1, CNTL2 and ASTC take writes; SRST resets the part.
static void MCLW_RAM_FUNC(ak_write)(struct mpu9250_state *s, uint8_t reg, uint8_t value) {
    if (reg == AK_CNTL2 && (value & AK_CNTL2_SRST) != 0) {
        ak_reset(s);
    } else if (reg == AK_CNTL1) {
        s->ak[AK_CNTL1] = value;
        s->ak_next_us = 0;
    } else if (reg == 0x0cu || reg == 0x0fu) {
        s->ak[reg] = value;
    }
}

static int16_t MCLW_RAM_FUNC(ak_counts)(int32_t nt, bool sixteen) {
    int32_t per = sixteen ? 150 : 600;
    int32_t c = (nt >= 0 ? nt + per / 2 : nt - per / 2) / per;

    return (int16_t)(c > INT16_MAX ? INT16_MAX : c < INT16_MIN ? INT16_MIN : c);
}

// One measurement into HXL..HZH, little-endian, with DRDY (DOR if the last
// was unread) and ST2's BITM.
static void MCLW_RAM_FUNC(ak_measure)(struct mpu9250_state *s) {
    bool sixteen = (s->ak[AK_CNTL1] & AK_CNTL1_BIT) != 0;

    for (uint8_t axis = 0; axis < 3; axis++) {
        uint16_t c = (uint16_t)ak_counts(s->magn_nt[axis], sixteen);
        s->ak[AK_HXL + 2u * axis] = (uint8_t)c;
        s->ak[AK_HXL + 2u * axis + 1u] = (uint8_t)(c >> 8);
    }
    s->ak[AK_ST2] = sixteen ? AK_ST2_BITM : 0;
    s->ak[AK_ST1] = (uint8_t)(AK_ST1_DRDY | ((s->ak[AK_ST1] & AK_ST1_DRDY) != 0 ? AK_ST1_DOR : 0u));
}

// Slave 4's single transfer (RM 4.13-4.17): to the AK8963 at 0Ch a read
// fills I2C_SLV4_DI, a write takes I2C_SLV4_DO; any other address NACKs.
// I2C_SLV4_EN clears and I2C_MST_STATUS reports it.
static void MCLW_RAM_FUNC(slv4_transfer)(struct mpu9250_state *s) {
    struct mclw_regmap *map = &s->map;
    uint8_t addr = mclw_regmap_get(map, 0, REG_I2C_SLV4_ADDR);
    uint8_t reg = mclw_regmap_get(map, 0, REG_I2C_SLV4_REG);
    uint8_t status = MST_STATUS_SLV4_DONE;

    if ((addr & 0x7fu) != AK_ADDRESS) {
        status = MST_STATUS_SLV4_NACK;
    } else if ((addr & SLV_RNW) != 0) {
        mclw_regmap_set(map, 0, REG_I2C_SLV4_DI, ak_read(s, reg), 0);
    } else {
        ak_write(s, reg, mclw_regmap_get(map, 0, REG_I2C_SLV4_DO));
    }
    mclw_regmap_set(map, 0, REG_I2C_SLV4_CTRL, (uint8_t)(mclw_regmap_get(map, 0, REG_I2C_SLV4_CTRL) & ~SLV_EN),
        MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, REG_I2C_MST_STATUS, status, MCLW_REG_CLEAR_ON_READ);
}

// Slave 0's read at each sample (RM 4.11-4.12, 4.21): LENG bytes from the
// AK8963 from I2C_SLV0_REG into EXT_SENS_DATA_00 on.
static void MCLW_RAM_FUNC(slv0_read)(struct mpu9250_state *s) {
    struct mclw_regmap *map = &s->map;
    uint8_t addr = mclw_regmap_get(map, 0, REG_I2C_SLV0_ADDR);
    uint8_t reg = mclw_regmap_get(map, 0, REG_I2C_SLV0_REG);
    uint8_t ctrl = mclw_regmap_get(map, 0, REG_I2C_SLV0_CTRL);

    if ((mclw_regmap_get(map, 0, REG_USER_CTRL) & USER_CTRL_I2C_MST_EN) == 0 || (ctrl & SLV_EN) == 0 ||
        addr != (SLV_RNW | AK_ADDRESS)) {
        return;
    }
    for (uint8_t i = 0; i < (ctrl & SLV0_LENG) && REG_EXT_SENS_DATA_00 + i <= REG_EXT_SENS_DATA_23; i++) {
        mclw_regmap_set(map, 0, (uint8_t)(REG_EXT_SENS_DATA_00 + i), ak_read(s, (uint8_t)(reg + i)), 0);
    }
}

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
    slv0_read(s);
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
    mclw_regmap_set(map, 0, REG_I2C_MST_STATUS, 0, MCLW_REG_CLEAR_ON_READ);
    publish(s);
}

static void MCLW_RAM_FUNC(mpu9250_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx) {
    struct mpu9250_state *s = ctx;

    (void)map;
    (void)bank;
    if (reg == REG_PWR_MGMT_1 && (value & PWR_MGMT_1_H_RESET) != 0) {
        reset_registers(s);
    } else if (reg == REG_I2C_SLV4_CTRL && (value & SLV_EN) != 0 &&
        (mclw_regmap_get(map, 0, REG_USER_CTRL) & USER_CTRL_I2C_MST_EN) != 0) {
        slv4_transfer(s);
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
    // A typical horizontal field: 20 uT north, 0 east, 40 uT down.
    s->magn_nt[0] = 20000;
    s->magn_nt[1] = 0;
    s->magn_nt[2] = 40000;
    ak_reset(s);
}

static int mpu9250_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct mpu9250_state *s = inst->state;

    if (bindings[BINDING_I2C].present == bindings[BINDING_SPI].present) {
        return -EINVAL;
    }
    s->i2c = bindings[BINDING_I2C].present ? bindings[BINDING_I2C].i2c : NULL;
    s->spi = bindings[BINDING_SPI].present ? bindings[BINDING_SPI].spi : NULL;
    s->int_pin = bindings[BINDING_INT].present ? bindings[BINDING_INT].gpio : NULL;
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
    s->int_pin = NULL;
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
        case PROP_MAGNETIC_FIELD:
            for (uint8_t axis = 0; axis < 3; axis++) {
                out->v[axis] = s->magn_nt[axis];
            }
            return 0;
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
        case PROP_MAGNETIC_FIELD:
            for (uint8_t axis = 0; axis < 3; axis++) {
                s->magn_nt[axis] = (int32_t)in->v[axis];
            }
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

// The AK8963's modes (MS1356-E-02 6.4): single measurement, then
// power-down; continuous 1 at 8 Hz and 2 at 100 Hz.
static void ak_tick(struct mpu9250_state *s, uint64_t now_us) {
    uint8_t mode = s->ak[AK_CNTL1] & 0x0fu;
    uint32_t period = mode == AK_MODE_CONT1 ? 125000u : mode == AK_MODE_CONT2 ? 10000u : 0u;

    if (mode == AK_MODE_SINGLE) {
        ak_measure(s);
        s->ak[AK_CNTL1] = (uint8_t)(s->ak[AK_CNTL1] & 0xf0u);
    } else if (period != 0 && now_us >= s->ak_next_us) {
        ak_measure(s);
        s->ak_next_us = now_us + period;
    }
}

// The INT pin (RM 4.18-4.19): with RAW_RDY_EN each sample emits a 50 us
// pulse, or with LATCH_INT_EN holds INT until INT_STATUS clears, at
// ACTL's level; push-pull either way (OPEN is not modelled).
static void drive_int(struct mpu9250_state *s, bool sampled) {
    struct mclw_regmap *map = &s->map;
    uint8_t cfg = mclw_regmap_get(map, 0, REG_INT_PIN_CFG);
    bool low = (cfg & INT_PIN_CFG_ACTL) != 0;
    bool latched = (cfg & INT_PIN_CFG_LATCH) != 0;
    bool enabled = (mclw_regmap_get(map, 0, REG_INT_ENABLE) & INT_ENABLE_RAW_RDY) != 0;
    bool ready = (mclw_regmap_get(map, 0, REG_INT_STATUS) & INT_STATUS_RAW_DATA_RDY) != 0;

    if (s->int_pin == NULL) {
        return;
    }
    if (sampled && enabled && !latched) {
        mclw_hal_gpio_write(s->int_pin, !low);
        mclw_hal_delay_us(INT_PULSE_US);
    }
    mclw_hal_gpio_write(s->int_pin, (enabled && latched && ready) != low);
}

// A new sample, and RAW_DATA_RDY_INT, every sample period while awake; the
// AK8963 measures on its own schedule.
static void mpu9250_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct mpu9250_state *s = inst->state;
    bool sampled = false;
    uint32_t critical = mclw_hal_critical_enter();

    ak_tick(s, now_us);
    if (now_us >= s->next_sample_us) {
        uint32_t divider = (uint32_t)mclw_regmap_get(&s->map, 0, REG_SMPLRT_DIV) + 1u;
        publish(s);
        s->next_sample_us = now_us + (uint64_t)divider * (1000000u / INTERNAL_RATE_HZ);
        sampled = (mclw_regmap_get(&s->map, 0, REG_PWR_MGMT_1) & PWR_MGMT_1_SLEEP) == 0;
    }
    mclw_hal_critical_exit(critical);
    drive_int(s, sampled);
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
