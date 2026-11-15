// SPDX-License-Identifier: Apache-2.0 OR MIT
// NXP MMA8451Q three-axis 14-bit accelerometer on I2C. NXP no longer serves
// the MMA8451Q data sheet; the register map and bits come from its sibling's
// data sheet (MMA8452Q Rev. 10, Table 11 and section 6), the family's data
// application note (AN4076 Rev. 1) and NXP's MCUXpresso driver for this
// part (fsl_mma.h, nxp-mcuxpresso/mcux-sdk 7ba15d9); see README.md.

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

#include "mma8451q.h"

// MMA8452Q Table 11 and fsl_mma.h.
#define REG_STATUS 0x00
#define REG_OUT_X_MSB 0x01
#define REG_OUT_Z_LSB 0x06
#define REG_F_SETUP 0x09
#define REG_SYSMOD 0x0b
#define REG_INT_SOURCE 0x0c
#define REG_WHO_AM_I 0x0d
#define REG_XYZ_DATA_CFG 0x0e
#define REG_PL_STATUS 0x10
#define REG_PL_CFG 0x11
#define REG_PL_COUNT 0x12
#define REG_PL_BF_ZCOMP 0x13
#define REG_P_L_THS 0x14
#define REG_FF_MT_SRC 0x16
#define REG_TRANSIENT_SRC 0x1e
#define REG_PULSE_SRC 0x22
#define REG_CTRL_REG1 0x2a
#define REG_CTRL_REG2 0x2b
#define REG_OFF_Z 0x31

// fsl_mma.h kMMA8451_WHO_AM_I_Device_ID.
#define MMA8451Q_ID 0x1a
#define CTRL_REG1_ACTIVE 0x01u
#define CTRL_REG1_F_READ 0x02u
#define CTRL_REG2_RST 0x40u
#define XYZ_DATA_CFG_FS 0x03u
#define STATUS_ZYXDR_XYZ 0x0fu
#define SYSMOD_WAKE 0x01u

// With F_READ set a burst skips the LSB registers: 0x00, 0x01, 0x03,
// 0x05 (MMA8452Q Table 11, auto-increment column). Those four bytes are
// copied behind the map, where the pointer map sends F_READ transfers.
#define FAST_READ_BASE 0x80

enum {
    PROP_ACCELERATION,
};

enum {
    BINDING_I2C,
};

struct mma8451q_state {
    struct mclw_regmap map;
    uint8_t pointers[MCLW_REGMAP_BANK_SIZE];
    struct mclw_i2c_target *i2c;
    int32_t accel_mg[3];
};

static const struct mclw_property_desc mma8451q_properties[] = {
    {
        .name = "acceleration",
        .type = MCLW_TYPE_I32,
        .count = 3,
        .access = MCLW_ACCESS_RW,
        .unit = "mg",
        .min = -8000,
        .max = 8000,
    },
};

static const struct mclw_binding_desc mma8451q_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static void MCLW_RAM_FUNC(route_pointers)(struct mma8451q_state *s) {
    bool fast = (mclw_regmap_get(&s->map, 0, REG_CTRL_REG1) & CTRL_REG1_F_READ) != 0;

    for (unsigned p = 0; p < MCLW_REGMAP_BANK_SIZE; p++) {
        s->pointers[p] = (uint8_t)p;
    }
    if (fast) {
        s->pointers[REG_STATUS] = FAST_READ_BASE;
        s->pointers[REG_OUT_X_MSB] = FAST_READ_BASE + 1;
        s->pointers[REG_OUT_X_MSB + 2] = FAST_READ_BASE + 2;
        s->pointers[REG_OUT_X_MSB + 4] = FAST_READ_BASE + 3;
    }
}

// 14-bit two's complement, left-justified over MSB and LSB, at 4096, 2048
// or 1024 counts/g for +-2/4/8 g (AN4076 section 2 and 7). Valid only in
// active mode (Table 11 note 2).
static void MCLW_RAM_FUNC(publish)(struct mma8451q_state *s) {
    struct mclw_regmap *map = &s->map;
    unsigned fs = mclw_regmap_get(map, 0, REG_XYZ_DATA_CFG) & XYZ_DATA_CFG_FS;
    int32_t per_g = 4096 >> (fs > 2 ? 2 : fs);

    if ((mclw_regmap_get(map, 0, REG_CTRL_REG1) & CTRL_REG1_ACTIVE) == 0) {
        return;
    }
    for (uint8_t axis = 0; axis < 3; axis++) {
        int64_t n = (int64_t)s->accel_mg[axis] * per_g;
        int32_t counts = (int32_t)(n >= 0 ? (n + 500) / 1000 : -((-n + 500) / 1000));
        counts = counts < -8192 ? -8192 : (counts > 8191 ? 8191 : counts);
        uint16_t word = (uint16_t)((uint32_t)counts << 2);
        mclw_regmap_set(map, 0, (uint8_t)(REG_OUT_X_MSB + 2 * axis), (uint8_t)(word >> 8), 0);
        mclw_regmap_set(map, 0, (uint8_t)(REG_OUT_X_MSB + 2 * axis + 1), (uint8_t)word, 0);
        mclw_regmap_set(map, 0, (uint8_t)(FAST_READ_BASE + 1 + axis), (uint8_t)(word >> 8), 0);
    }
    mclw_regmap_set(map, 0, REG_STATUS, STATUS_ZYXDR_XYZ, 0);
    mclw_regmap_set(map, 0, FAST_READ_BASE, STATUS_ZYXDR_XYZ, 0);
}

// MMA8452Q Table 11 defaults: everything zero but PL_CFG, PL_BF_ZCOMP and
// P_L_THS_REG; status, output, source registers and WHO_AM_I read-only.
static void MCLW_RAM_FUNC(reset_registers)(struct mma8451q_state *s) {
    struct mclw_regmap *map = &s->map;

    for (unsigned reg = 0; reg < MCLW_REGMAP_BANK_SIZE; reg++) {
        bool control = reg >= REG_F_SETUP && reg <= REG_OFF_Z && reg != REG_SYSMOD && reg != REG_INT_SOURCE
            && reg != REG_WHO_AM_I && reg != REG_PL_STATUS && reg != REG_PL_COUNT && reg != REG_FF_MT_SRC
            && reg != REG_TRANSIENT_SRC && reg != REG_PULSE_SRC;
        mclw_regmap_set(map, 0, (uint8_t)reg, 0, control ? MCLW_REG_WRITABLE : 0);
    }
    mclw_regmap_set(map, 0, REG_WHO_AM_I, MMA8451Q_ID, 0);
    mclw_regmap_set(map, 0, REG_PL_CFG, 0x80, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, REG_PL_BF_ZCOMP, 0x44, 0);
    mclw_regmap_set(map, 0, REG_P_L_THS, 0x84, 0);
    route_pointers(s);
}

static void MCLW_RAM_FUNC(mma8451q_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx) {
    struct mma8451q_state *s = ctx;

    (void)map;
    (void)bank;
    if (reg == REG_CTRL_REG2 && (value & CTRL_REG2_RST) != 0) {
        reset_registers(s);
        return;
    }
    if (reg == REG_CTRL_REG1) {
        // Output registers reset on the standby-to-active transition (Table
        // 11 note 1); the first sample is there at once.
        bool active = (value & CTRL_REG1_ACTIVE) != 0;
        mclw_regmap_set(&s->map, 0, REG_SYSMOD, active ? SYSMOD_WAKE : 0, 0);
        route_pointers(s);
        publish(s);
    } else if (reg == REG_XYZ_DATA_CFG) {
        publish(s);
    }
}

static void build_map(struct mma8451q_state *s) {
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    reset_registers(s);
    mclw_regmap_set_pointer_map(&s->map, s->pointers);
    mclw_regmap_set_on_write(&s->map, mma8451q_on_write, s);
}

static void defaults(struct mma8451q_state *s) {
    s->accel_mg[0] = 0;
    s->accel_mg[1] = 0;
    s->accel_mg[2] = 1000;
}

static int mma8451q_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct mma8451q_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    defaults(s);
    build_map(s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void mma8451q_deinit(struct mclw_instance *inst) {
    struct mma8451q_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int mma8451q_reset(struct mclw_instance *inst, bool factory) {
    struct mma8451q_state *s = inst->state;

    (void)factory;
    uint32_t critical = mclw_hal_critical_enter();
    defaults(s);
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int mma8451q_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct mma8451q_state *s = inst->state;

    if (property != PROP_ACCELERATION) {
        return -EINVAL;
    }
    for (uint8_t axis = 0; axis < 3; axis++) {
        out->v[axis] = s->accel_mg[axis];
    }
    return 0;
}

static int mma8451q_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct mma8451q_state *s = inst->state;

    if (property != PROP_ACCELERATION) {
        return -EINVAL;
    }
    for (uint8_t axis = 0; axis < 3; axis++) {
        s->accel_mg[axis] = (int32_t)in->v[axis];
    }
    uint32_t critical = mclw_hal_critical_enter();
    publish(s);
    mclw_hal_critical_exit(critical);
    mclw_trace(inst, MCLW_TRACE_STATE, REG_OUT_X_MSB, REG_OUT_Z_LSB, 0);
    return 0;
}

static const struct mclw_model_ops mma8451q_ops = {
    .init = mma8451q_init,
    .deinit = mma8451q_deinit,
    .reset = mma8451q_reset,
    .get = mma8451q_get,
    .set = mma8451q_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_mma8451q = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "mma8451q",
    .family = "mma8451q",
    .config = NULL,
    .properties = mma8451q_properties,
    .property_count = sizeof(mma8451q_properties) / sizeof(mma8451q_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = mma8451q_bindings,
    .binding_count = sizeof(mma8451q_bindings) / sizeof(mma8451q_bindings[0]),
    .state_size = sizeof(struct mma8451q_state),
    .ops = &mma8451q_ops,
};
