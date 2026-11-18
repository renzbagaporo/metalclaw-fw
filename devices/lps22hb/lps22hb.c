// SPDX-License-Identifier: Apache-2.0 OR MIT
// ST LPS22HB MEMS nano pressure sensor on I2C: a register map the harness
// serves from hardware, converting on the tick at the ODR or once per
// ONE_SHOT. Section and table numbers are the datasheet's, DocID027083 Rev 5.

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

// Table 16, registers address map.
#define REG_INTERRUPT_CFG 0x0bu
#define REG_WHO_AM_I 0x0fu
#define REG_CTRL_REG1 0x10u
#define REG_CTRL_REG2 0x11u
#define REG_RES_CONF 0x1au
#define REG_INT_SOURCE 0x25u
#define REG_FIFO_STATUS 0x26u
#define REG_STATUS 0x27u
#define REG_PRESS_OUT_XL 0x28u
#define REG_TEMP_OUT_L 0x2bu
#define REG_LPFP_RES 0x33u
#define WHO_AM_I_LPS22HB 0xb1u
// CTRL_REG1 (9.6): ODR[2:0] in bits 6:4. CTRL_REG2 (9.7): BOOT 7, SWRESET 2,
// ONE_SHOT 0; reset value 10h (IF_ADD_INC). STATUS (9.18): T_DA 1, P_DA 0.
#define CTRL_REG2_RESET 0x10u
#define CTRL_REG2_BOOT 0x80u
#define CTRL_REG2_SWRESET 0x04u
#define CTRL_REG2_ONE_SHOT 0x01u
#define STATUS_DA 0x03u

enum {
    PROP_PRESSURE,
    PROP_TEMPERATURE,
    PROP_CONVERSIONS,
};

enum {
    BINDING_I2C,
};

struct lps22hb_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    int32_t pressure_pa;
    int32_t temperature_mc;
    uint32_t conversions;
    uint64_t next_us;
};

static const struct mclw_property_desc lps22hb_properties[] = {
    { .name = "pressure", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "Pa",
      .min = 26000, .max = 126000 },
    { .name = "temperature", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mC",
      .min = -40000, .max = 85000 },
    { .name = "conversions", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc lps22hb_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static uint8_t get(const struct lps22hb_state *s, uint8_t reg) {
    return mclw_regmap_get(&s->map, 0, reg);
}

// Table 16's reset values: everything zero but WHO_AM_I (B1h) and
// CTRL_REG2 (10h); the outputs and status registers are read-only.
static void MCLW_RAM_FUNC(reset_registers)(struct lps22hb_state *s) {
    for (uint16_t reg = REG_INTERRUPT_CFG; reg <= REG_LPFP_RES; reg++) {
        bool ro = reg == REG_WHO_AM_I || reg >= REG_INT_SOURCE;
        mclw_regmap_set(&s->map, 0, (uint8_t)reg, 0, ro ? 0 : MCLW_REG_WRITABLE);
    }
    mclw_regmap_set(&s->map, 0, REG_WHO_AM_I, WHO_AM_I_LPS22HB, 0);
    mclw_regmap_set(&s->map, 0, REG_CTRL_REG2, CTRL_REG2_RESET, MCLW_REG_WRITABLE);
}

// 4.4 and 4.5: pressure in 24 bits of 1/4096 hPa, temperature in 16 bits
// of 1/100 C, two's complement, little-endian from the XL byte.
static void MCLW_RAM_FUNC(convert)(struct lps22hb_state *s) {
    int64_t p = (int64_t)s->pressure_pa * 4096 / 100;
    int32_t t = s->temperature_mc / 10;

    mclw_regmap_set_le24(&s->map, 0, REG_PRESS_OUT_XL, (uint32_t)p & 0xffffffu);
    mclw_regmap_set_le16(&s->map, 0, REG_TEMP_OUT_L, (uint16_t)(int16_t)t);
    mclw_regmap_set(&s->map, 0, REG_STATUS, (uint8_t)(get(s, REG_STATUS) | STATUS_DA), 0);
    s->conversions++;
}

// SWRESET and BOOT reset the registers and clear themselves (9.7).
static void MCLW_RAM_FUNC(lps22hb_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct lps22hb_state *s = ctx;

    (void)map;
    (void)bank;
    if (reg == REG_CTRL_REG2 && (value & (CTRL_REG2_SWRESET | CTRL_REG2_BOOT)) != 0) {
        reset_registers(s);
    }
}

// 9.6, table 17: ODR 000 is power-down, where ONE_SHOT takes one
// conversion and clears itself (9.7); 001-101 are 1, 10, 25, 50 and 75 Hz.
static uint32_t period_us(uint8_t ctrl1) {
    static const uint32_t period[] = { 0, 1000000, 100000, 40000, 20000, 13333, 13333, 13333 };

    return period[(ctrl1 >> 4) & 7u];
}

static int lps22hb_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct lps22hb_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->pressure_pa = 101325;
    s->temperature_mc = 25000;
    s->conversions = 0;
    s->next_us = 0;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    mclw_regmap_set_on_write(&s->map, lps22hb_on_write, s);
    uint32_t critical = mclw_hal_critical_enter();
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void lps22hb_deinit(struct mclw_instance *inst) {
    struct lps22hb_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int lps22hb_reset(struct mclw_instance *inst, bool factory) {
    struct lps22hb_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    s->conversions = 0;
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int lps22hb_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct lps22hb_state *s = inst->state;

    switch (property) {
        case PROP_PRESSURE:
            out->v[0] = s->pressure_pa;
            return 0;
        case PROP_TEMPERATURE:
            out->v[0] = s->temperature_mc;
            return 0;
        case PROP_CONVERSIONS:
            out->v[0] = s->conversions;
            return 0;
        default:
            return -EINVAL;
    }
}

static int lps22hb_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct lps22hb_state *s = inst->state;

    switch (property) {
        case PROP_PRESSURE:
            s->pressure_pa = (int32_t)in->v[0];
            return 0;
        case PROP_TEMPERATURE:
            s->temperature_mc = (int32_t)in->v[0];
            return 0;
        default:
            return -EINVAL;
    }
}

static void lps22hb_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct lps22hb_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();
    uint8_t ctrl1 = get(s, REG_CTRL_REG1);
    uint8_t ctrl2 = get(s, REG_CTRL_REG2);
    uint32_t period = period_us(ctrl1);

    if (period == 0) {
        if ((ctrl2 & CTRL_REG2_ONE_SHOT) != 0) {
            convert(s);
            mclw_regmap_set(&s->map, 0, REG_CTRL_REG2, (uint8_t)(ctrl2 & ~CTRL_REG2_ONE_SHOT), MCLW_REG_WRITABLE);
        }
        s->next_us = now_us;
    } else if (now_us >= s->next_us) {
        convert(s);
        s->next_us = now_us + period;
    }
    mclw_hal_critical_exit(critical);
}

static const struct mclw_model_ops lps22hb_ops = {
    .init = lps22hb_init,
    .deinit = lps22hb_deinit,
    .reset = lps22hb_reset,
    .get = lps22hb_get,
    .set = lps22hb_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = lps22hb_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_lps22hb = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "lps22hb",
    .family = "lps22hb",
    .config = NULL,
    .properties = lps22hb_properties,
    .property_count = sizeof(lps22hb_properties) / sizeof(lps22hb_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = lps22hb_bindings,
    .binding_count = sizeof(lps22hb_bindings) / sizeof(lps22hb_bindings[0]),
    .state_size = sizeof(struct lps22hb_state),
    .ops = &lps22hb_ops,
};
