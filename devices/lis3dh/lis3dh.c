// SPDX-License-Identifier: Apache-2.0 OR MIT
// ST LIS3DH three-axis accelerometer on I2C. ST's datasheet could not be
// downloaded (st.com refuses scripted clients); the register map, bit
// fields and data scaling come from ST's own driver, STMicroelectronics/
// lis3dh-pid 8c0588b (lis3dh_reg.h, lis3dh_reg.c); see README.md.

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

#include "lis3dh.h"

// lis3dh_reg.h
#define REG_STATUS_REG_AUX 0x07
#define REG_OUT_ADC1_L 0x08
#define REG_OUT_ADC3_L 0x0c
#define REG_WHO_AM_I 0x0f
#define REG_CTRL_REG0 0x1e
#define REG_TEMP_CFG_REG 0x1f
#define REG_CTRL_REG1 0x20
#define REG_CTRL_REG4 0x23
#define REG_CTRL_REG5 0x24
#define REG_STATUS_REG 0x27
#define REG_OUT_X_L 0x28
#define REG_FIFO_SRC_REG 0x2f
#define REG_INT1_SRC 0x31
#define REG_INT2_SRC 0x35
#define REG_CLICK_SRC 0x39
#define REG_LAST 0x3f

#define LIS3DH_ID 0x33
#define CTRL_REG1_LPEN 0x08u
#define CTRL_REG1_ODR_SHIFT 4
#define CTRL_REG4_BLE 0x40u
#define CTRL_REG4_HR 0x08u
#define CTRL_REG4_FS_SHIFT 4
#define CTRL_REG5_BOOT 0x80u
#define TEMP_CFG_ADC_PD 0x80u
#define TEMP_CFG_TEMP_EN 0x40u
#define STATUS_ZYXDA_XYZ 0x0fu
#define STATUS_AUX_321DA 0x0fu

// Zephyr's lis2dh driver, which binds this part, sets bit 7 of the
// register address for a multi-byte I2C access (LIS2DH_AUTOINCREMENT_ADDR,
// drivers/sensor/st/lis2dh/lis2dh.h).
#define SUB_AUTO_INCREMENT 0x80u

enum {
    PROP_ACCELERATION,
    PROP_TEMPERATURE,
};

enum {
    BINDING_I2C,
};

struct lis3dh_state {
    struct mclw_regmap map;
    uint8_t pointers[MCLW_REGMAP_BANK_SIZE];
    struct mclw_i2c_target *i2c;
    int32_t accel_mg[3];
    int32_t temperature_mc;
};

static const struct mclw_property_desc lis3dh_properties[] = {
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
        .name = "temperature",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mC",
        .min = -40000,
        .max = 85000,
    },
};

static const struct mclw_binding_desc lis3dh_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static int32_t MCLW_RAM_FUNC(div_round)(int64_t n, int64_t d) {
    return (int32_t)(n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d));
}

static void MCLW_RAM_FUNC(put16)(struct lis3dh_state *s, uint8_t reg, int32_t value, bool big_endian) {
    uint16_t v = (uint16_t)(int16_t)(value < INT16_MIN ? INT16_MIN : (value > INT16_MAX ? INT16_MAX : value));

    mclw_regmap_set(&s->map, 0, reg, (uint8_t)(big_endian ? v >> 8 : v), 0);
    mclw_regmap_set(&s->map, 0, (uint8_t)(reg + 1u), (uint8_t)(big_endian ? v : v >> 8), 0);
}

// The output is left-justified in 16 bits: 8 bits in low-power mode, 10 in
// normal mode, 12 in high-resolution mode, at 16/32/64/192, 4/8/16/48 and
// 1/2/4/12 mg per digit for +-2/4/8/16 g; the temperature is 8 bits in
// low-power mode and 10 otherwise, 1 or 4 digits per C from 25 C
// (lis3dh_from_fs*_{lp,nm,hr}_to_mg, lis3dh_from_lsb_*_to_celsius).
static void MCLW_RAM_FUNC(publish)(struct lis3dh_state *s) {
    static const uint8_t lp_mg[4] = { 16, 32, 64, 192 };
    static const uint8_t nm_mg[4] = { 4, 8, 16, 48 };
    static const uint8_t hr_mg[4] = { 1, 2, 4, 12 };
    struct mclw_regmap *map = &s->map;
    uint8_t ctrl1 = mclw_regmap_get(map, 0, REG_CTRL_REG1);
    uint8_t ctrl4 = mclw_regmap_get(map, 0, REG_CTRL_REG4);
    uint8_t temp_cfg = mclw_regmap_get(map, 0, REG_TEMP_CFG_REG);
    unsigned fs = (ctrl4 >> CTRL_REG4_FS_SHIFT) & 3u;
    bool low_power = (ctrl1 & CTRL_REG1_LPEN) != 0;
    bool high_res = !low_power && (ctrl4 & CTRL_REG4_HR) != 0;
    bool big_endian = (ctrl4 & CTRL_REG4_BLE) != 0;
    unsigned shift = low_power ? 8 : (high_res ? 4 : 6);
    unsigned mg = low_power ? lp_mg[fs] : (high_res ? hr_mg[fs] : nm_mg[fs]);

    // ODR 0 is power-down: the outputs hold.
    if ((ctrl1 >> CTRL_REG1_ODR_SHIFT) == 0) {
        return;
    }
    for (uint8_t axis = 0; axis < 3; axis++) {
        int32_t digits = div_round(s->accel_mg[axis], mg);
        put16(s, (uint8_t)(REG_OUT_X_L + 2 * axis), digits * (1 << shift), big_endian);
    }
    mclw_regmap_set(map, 0, REG_STATUS_REG, STATUS_ZYXDA_XYZ, 0);
    if ((temp_cfg & TEMP_CFG_ADC_PD) != 0) {
        int32_t per_c = low_power ? 1 : 4;
        int32_t temp = (temp_cfg & TEMP_CFG_TEMP_EN) != 0
            ? div_round(((int64_t)s->temperature_mc - 25000) * per_c, 1000) * (1 << (low_power ? 8 : 6))
            : 0;
        put16(s, REG_OUT_ADC3_L, temp, big_endian);
        mclw_regmap_set(map, 0, REG_STATUS_REG_AUX, STATUS_AUX_321DA, 0);
    }
}

// Every control register zero and writable; the outputs, the status and
// source registers and WHO_AM_I read-only.
static void MCLW_RAM_FUNC(reset_registers)(struct lis3dh_state *s) {
    struct mclw_regmap *map = &s->map;

    for (unsigned reg = 0; reg < MCLW_REGMAP_BANK_SIZE; reg++) {
        bool control = reg >= REG_CTRL_REG0 && reg <= REG_LAST && reg != REG_STATUS_REG
            && !(reg >= REG_OUT_X_L && reg <= REG_FIFO_SRC_REG) && reg != REG_INT1_SRC
            && reg != REG_INT2_SRC && reg != REG_CLICK_SRC;
        mclw_regmap_set(map, 0, (uint8_t)reg, 0, control ? MCLW_REG_WRITABLE : 0);
    }
    mclw_regmap_set(map, 0, REG_WHO_AM_I, LIS3DH_ID, 0);
}

static void MCLW_RAM_FUNC(lis3dh_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx) {
    struct lis3dh_state *s = ctx;

    (void)map;
    (void)bank;
    // CTRL_REG5 BOOT reloads the trimming and clears itself (lis3dh_boot_set).
    if (reg == REG_CTRL_REG5 && (value & CTRL_REG5_BOOT) != 0) {
        mclw_regmap_set(&s->map, 0, REG_CTRL_REG5, (uint8_t)(value & ~CTRL_REG5_BOOT), MCLW_REG_WRITABLE);
    }
    if (reg == REG_CTRL_REG1 || reg == REG_CTRL_REG4 || reg == REG_TEMP_CFG_REG) {
        publish(s);
    }
}

static void build_map(struct lis3dh_state *s) {
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    reset_registers(s);
    for (unsigned p = 0; p < MCLW_REGMAP_BANK_SIZE; p++) {
        s->pointers[p] = (uint8_t)(p & ~SUB_AUTO_INCREMENT);
    }
    mclw_regmap_set_pointer_map(&s->map, s->pointers);
    mclw_regmap_set_on_write(&s->map, lis3dh_on_write, s);
}

static void defaults(struct lis3dh_state *s) {
    s->accel_mg[0] = 0;
    s->accel_mg[1] = 0;
    s->accel_mg[2] = 1000;
    s->temperature_mc = 25000;
}

static int lis3dh_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct lis3dh_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    defaults(s);
    build_map(s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void lis3dh_deinit(struct mclw_instance *inst) {
    struct lis3dh_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int lis3dh_reset(struct mclw_instance *inst, bool factory) {
    struct lis3dh_state *s = inst->state;

    (void)factory;
    uint32_t critical = mclw_hal_critical_enter();
    defaults(s);
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int lis3dh_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct lis3dh_state *s = inst->state;

    switch (property) {
        case PROP_ACCELERATION:
            for (uint8_t axis = 0; axis < 3; axis++) {
                out->v[axis] = s->accel_mg[axis];
            }
            return 0;
        case PROP_TEMPERATURE:
            out->v[0] = s->temperature_mc;
            return 0;
        default:
            return -EINVAL;
    }
}

static int lis3dh_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct lis3dh_state *s = inst->state;

    switch (property) {
        case PROP_ACCELERATION:
            for (uint8_t axis = 0; axis < 3; axis++) {
                s->accel_mg[axis] = (int32_t)in->v[axis];
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
    mclw_trace(inst, MCLW_TRACE_STATE, REG_OUT_X_L, 6, 0);
    return 0;
}

static const struct mclw_model_ops lis3dh_ops = {
    .init = lis3dh_init,
    .deinit = lis3dh_deinit,
    .reset = lis3dh_reset,
    .get = lis3dh_get,
    .set = lis3dh_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_lis3dh = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "lis3dh",
    .family = "lis3dh",
    .config = NULL,
    .properties = lis3dh_properties,
    .property_count = sizeof(lis3dh_properties) / sizeof(lis3dh_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = lis3dh_bindings,
    .binding_count = sizeof(lis3dh_bindings) / sizeof(lis3dh_bindings[0]),
    .state_size = sizeof(struct lis3dh_state),
    .ops = &lis3dh_ops,
};
