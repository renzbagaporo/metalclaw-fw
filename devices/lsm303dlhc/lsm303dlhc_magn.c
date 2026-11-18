// SPDX-License-Identifier: Apache-2.0 OR MIT
// The magnetometer of ST's LSM303DLHC on I2C, at its own address (1Eh):
// registers 00h-0Ch and 31h-32h (Table 17) as a register map the harness
// serves from hardware, with the field converted at the gain CRB_REG_M
// selects. Data sheet Doc ID 018771 Rev 1, section 7.2.

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

#define REG_CRA 0x00
#define REG_CRB 0x01
#define REG_MR 0x02
#define REG_OUT_X_H 0x03
#define REG_OUT_Z_H 0x05
#define REG_OUT_Y_H 0x07
#define REG_SR 0x09
#define REG_IRA 0x0a
#define REG_IRB 0x0b
#define REG_IRC 0x0c
#define REG_TEMP_OUT_H 0x31

// Table 17's defaults.
#define CRA_RESET 0x10
#define CRB_RESET 0x20
#define MR_RESET 0x03
#define CRA_TEMP_EN 0x80
#define CRA_DO_SHIFT 2
#define MR_MODE 0x03
#define MR_CONTINUOUS 0x00
#define MR_SINGLE 0x01
#define SR_DRDY 0x01

#define NT_PER_GAUSS 100000

enum {
    PROP_MAGNETIC_FIELD,
    PROP_TEMPERATURE,
};

enum {
    BINDING_I2C,
};

struct lsm303dlhc_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    int32_t field_nt[3];
    int32_t temperature_mc;
    uint64_t next_us;
    volatile bool single;
    volatile bool start;
};

static const struct mclw_property_desc lsm303dlhc_properties[] = {
    { .name = "magnetic_field", .type = MCLW_TYPE_I32, .count = 3, .access = MCLW_ACCESS_RW, .unit = "nT",
      .min = -810000, .max = 810000 },
    { .name = "temperature", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mC",
      .min = -40000, .max = 85000 },
};

static const struct mclw_binding_desc lsm303dlhc_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static uint8_t get(const struct lsm303dlhc_state *s, uint8_t reg) {
    return mclw_regmap_get(&s->map, 0, reg);
}

static void put(struct lsm303dlhc_state *s, uint8_t reg, uint8_t value, uint8_t flags) {
    mclw_regmap_set(&s->map, 0, reg, value, flags);
}

// Table 75: LSB/gauss for X and Y, then Z, by GN2-GN0 (000 is not
// listed; it reads as 001 here).
static const uint16_t gain_xy[8] = { 1100, 1100, 855, 670, 450, 400, 330, 230 };
static const uint16_t gain_z[8] = { 980, 980, 760, 600, 400, 355, 295, 205 };

static int16_t counts(int32_t nt, uint16_t lsb_per_gauss) {
    int64_t num = (int64_t)nt * lsb_per_gauss;
    int64_t c = num >= 0 ? (num + NT_PER_GAUSS / 2) / NT_PER_GAUSS : (num - NT_PER_GAUSS / 2) / NT_PER_GAUSS;

    // Table 75's output range, F800h-07FFh.
    if (c > 2047) {
        c = 2047;
    } else if (c < -2048) {
        c = -2048;
    }
    return (int16_t)c;
}

// One measurement: X, Z and Y high byte first (Table 17's order), the
// temperature (8 LSB/C, 12 bits left-justified, 7.2.9) when enabled, then
// DRDY.
static void measure(struct lsm303dlhc_state *s) {
    uint8_t gn = (uint8_t)(get(s, REG_CRB) >> 5);
    int16_t x = counts(s->field_nt[0], gain_xy[gn]);
    int16_t y = counts(s->field_nt[1], gain_xy[gn]);
    int16_t z = counts(s->field_nt[2], gain_z[gn]);
    uint32_t critical = mclw_hal_critical_enter();

    mclw_regmap_set_be16(&s->map, 0, REG_OUT_X_H, (uint16_t)x);
    mclw_regmap_set_be16(&s->map, 0, REG_OUT_Z_H, (uint16_t)z);
    mclw_regmap_set_be16(&s->map, 0, REG_OUT_Y_H, (uint16_t)y);
    if ((get(s, REG_CRA) & CRA_TEMP_EN) != 0) {
        int32_t t = s->temperature_mc * 8 / 1000;
        mclw_regmap_set_be16(&s->map, 0, REG_TEMP_OUT_H, (uint16_t)(int16_t)(t << 4));
    }
    put(s, REG_SR, SR_DRDY, 0);
    mclw_hal_critical_exit(critical);
}

// MR_REG_M: single-conversion mode measures once, then the part sleeps
// (Table 78); continuous-conversion mode measures at once and then at the
// rate. Table 72 gives only minimum rates, not when the first set lands;
// Zephyr's magn_polling, which lists the stm32f3_disco's LSM303DLHC, reads
// DRDY as soon as its driver has set the mode.
static void MCLW_RAM_FUNC(lsm303dlhc_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx) {
    struct lsm303dlhc_state *s = ctx;

    (void)map;
    (void)bank;
    if (reg == REG_MR && (value & MR_MODE) == MR_SINGLE) {
        s->single = true;
    } else if (reg == REG_MR && (value & MR_MODE) == MR_CONTINUOUS) {
        s->start = true;
    }
}

static void power_on(struct lsm303dlhc_state *s) {
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    put(s, REG_CRA, CRA_RESET, MCLW_REG_WRITABLE);
    put(s, REG_CRB, CRB_RESET, MCLW_REG_WRITABLE);
    put(s, REG_MR, MR_RESET, MCLW_REG_WRITABLE);
    put(s, REG_SR, 0x00, 0);
    put(s, REG_IRA, 0x48, 0);
    put(s, REG_IRB, 0x34, 0);
    put(s, REG_IRC, 0x33, 0);
    s->next_us = 0;
    s->single = false;
    s->start = false;
}

static int lsm303dlhc_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct lsm303dlhc_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->field_nt[0] = 0;
    s->field_nt[1] = 0;
    s->field_nt[2] = 0;
    s->temperature_mc = 25000;
    power_on(s);
    mclw_regmap_set_on_write(&s->map, lsm303dlhc_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void lsm303dlhc_deinit(struct mclw_instance *inst) {
    struct lsm303dlhc_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int lsm303dlhc_reset(struct mclw_instance *inst, bool factory) {
    struct lsm303dlhc_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    power_on(s);
    mclw_regmap_set_on_write(&s->map, lsm303dlhc_on_write, s);
    mclw_hal_critical_exit(critical);
    return 0;
}

// Table 72's rates, as periods: 0.75 Hz to 220 Hz.
static void lsm303dlhc_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    static const uint32_t period_us[8] = { 1333333u, 666667u, 333333u, 133333u, 66667u, 33333u, 13333u, 4545u };
    struct lsm303dlhc_state *s = inst->state;
    uint32_t period;

    if (s->single) {
        uint32_t critical = mclw_hal_critical_enter();

        s->single = false;
        put(s, REG_MR, (uint8_t)((get(s, REG_MR) & ~MR_MODE) | MR_RESET), MCLW_REG_WRITABLE);
        mclw_hal_critical_exit(critical);
        measure(s);
    }
    if ((get(s, REG_MR) & MR_MODE) != MR_CONTINUOUS) {
        s->next_us = 0;
        return;
    }
    period = period_us[(get(s, REG_CRA) >> CRA_DO_SHIFT) & 7u];
    if (s->start || s->next_us == 0) {
        s->start = false;
        s->next_us = now_us;
    }
    if (now_us >= s->next_us) {
        s->next_us += period;
        if (s->next_us <= now_us) {
            s->next_us = now_us + period;
        }
        measure(s);
    }
}

static int lsm303dlhc_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct lsm303dlhc_state *s = inst->state;

    switch (property) {
        case PROP_MAGNETIC_FIELD:
            out->v[0] = s->field_nt[0];
            out->v[1] = s->field_nt[1];
            out->v[2] = s->field_nt[2];
            return 0;
        case PROP_TEMPERATURE:
            out->v[0] = s->temperature_mc;
            return 0;
        default:
            return -EINVAL;
    }
}

static int lsm303dlhc_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct lsm303dlhc_state *s = inst->state;

    switch (property) {
        case PROP_MAGNETIC_FIELD:
            s->field_nt[0] = (int32_t)in->v[0];
            s->field_nt[1] = (int32_t)in->v[1];
            s->field_nt[2] = (int32_t)in->v[2];
            return 0;
        case PROP_TEMPERATURE:
            s->temperature_mc = (int32_t)in->v[0];
            return 0;
        default:
            return -EINVAL;
    }
}

static const struct mclw_model_ops lsm303dlhc_ops = {
    .init = lsm303dlhc_init,
    .deinit = lsm303dlhc_deinit,
    .reset = lsm303dlhc_reset,
    .get = lsm303dlhc_get,
    .set = lsm303dlhc_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = lsm303dlhc_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_lsm303dlhc_magn = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "lsm303dlhc_magn",
    .family = "lsm303dlhc",
    .config = NULL,
    .properties = lsm303dlhc_properties,
    .property_count = sizeof(lsm303dlhc_properties) / sizeof(lsm303dlhc_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = lsm303dlhc_bindings,
    .binding_count = sizeof(lsm303dlhc_bindings) / sizeof(lsm303dlhc_bindings[0]),
    .state_size = sizeof(struct lsm303dlhc_state),
    .ops = &lsm303dlhc_ops,
};
