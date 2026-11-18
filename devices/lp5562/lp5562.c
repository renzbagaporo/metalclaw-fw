// SPDX-License-Identifier: Apache-2.0 OR MIT
// TI LP5562 4-channel RGBW LED driver on I2C: the register map of 7.6,
// 00h-70h, at its reset values, a register map the harness serves from
// hardware, with the RESET register's software reset. The program engines
// keep what is written to their memory but do not run. Data sheet SNVS820B.

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

#define REG_ENABLE 0x00
#define REG_OP_MODE 0x01
#define REG_B_PWM 0x02
#define REG_G_PWM 0x03
#define REG_R_PWM 0x04
#define REG_B_CURRENT 0x05
#define REG_R_CURRENT 0x07
#define REG_RESET 0x0d
#define REG_W_PWM 0x0e
#define REG_W_CURRENT 0x0f
#define REG_PROG_MEM 0x10
#define REG_LED_MAP 0x70
#define ENABLE_CHIP_EN 0x40
#define RESET_SW 0xff
#define CURRENT_RESET 0xaf
#define LED_MAP_RESET 0x39

enum {
    PROP_ENABLED,
    PROP_PWM,
    PROP_LED_MAP,
    PROP_OP_MODE,
    PROP_WRITES,
};

enum {
    BINDING_I2C,
};

struct lp5562_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    volatile uint32_t writes;
};

static const struct mclw_property_desc lp5562_properties[] = {
    { .name = "enabled", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "pwm", .type = MCLW_TYPE_U32, .count = 4, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "led_map", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "op_mode", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "writes", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc lp5562_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// 7.6's defaults: B, G, R and W CURRENT AFh (17.5 mA), LED MAP 39h, the
// rest 00h, program memory included. RESET reads 00h.
static void load_defaults(struct mclw_regmap *map) {
    for (uint16_t reg = 0; reg <= REG_LED_MAP; reg++) {
        uint8_t value = 0x00;

        if ((reg >= REG_B_CURRENT && reg <= REG_R_CURRENT) || reg == REG_W_CURRENT) {
            value = CURRENT_RESET;
        } else if (reg == REG_LED_MAP) {
            value = LED_MAP_RESET;
        }
        mclw_regmap_set(map, 0, (uint8_t)reg, value, MCLW_REG_WRITABLE);
    }
}

// 7.6.14: FFh in RESET resets every register to its default.
static void MCLW_RAM_FUNC(lp5562_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct lp5562_state *s = ctx;

    (void)bank;
    s->writes = s->writes + 1u;
    if (reg == REG_RESET) {
        if (value == RESET_SW) {
            load_defaults(map);
        } else {
            mclw_regmap_set(map, 0, REG_RESET, 0x00, MCLW_REG_WRITABLE);
        }
    }
}

static int lp5562_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct lp5562_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->writes = 0;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    load_defaults(&s->map);
    mclw_regmap_set_on_write(&s->map, lp5562_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void lp5562_deinit(struct mclw_instance *inst) {
    struct lp5562_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int lp5562_reset(struct mclw_instance *inst, bool factory) {
    struct lp5562_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    load_defaults(&s->map);
    mclw_hal_critical_exit(critical);
    s->writes = 0;
    return 0;
}

static int lp5562_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct lp5562_state *s = inst->state;

    switch (property) {
        case PROP_ENABLED:
            out->v[0] = (mclw_regmap_get(&s->map, 0, REG_ENABLE) & ENABLE_CHIP_EN) != 0;
            return 0;
        case PROP_PWM:
            // R, G, B, W.
            out->v[0] = mclw_regmap_get(&s->map, 0, REG_R_PWM);
            out->v[1] = mclw_regmap_get(&s->map, 0, REG_G_PWM);
            out->v[2] = mclw_regmap_get(&s->map, 0, REG_B_PWM);
            out->v[3] = mclw_regmap_get(&s->map, 0, REG_W_PWM);
            return 0;
        case PROP_LED_MAP:
            out->v[0] = mclw_regmap_get(&s->map, 0, REG_LED_MAP);
            return 0;
        case PROP_OP_MODE:
            out->v[0] = mclw_regmap_get(&s->map, 0, REG_OP_MODE);
            return 0;
        case PROP_WRITES:
            out->v[0] = s->writes;
            return 0;
        default:
            return -EINVAL;
    }
}

static int lp5562_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    (void)inst;
    (void)property;
    (void)in;
    return -EINVAL;
}

static const struct mclw_model_ops lp5562_ops = {
    .init = lp5562_init,
    .deinit = lp5562_deinit,
    .reset = lp5562_reset,
    .get = lp5562_get,
    .set = lp5562_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_lp5562 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "lp5562",
    .family = "lp5562",
    .config = NULL,
    .properties = lp5562_properties,
    .property_count = sizeof(lp5562_properties) / sizeof(lp5562_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = lp5562_bindings,
    .binding_count = sizeof(lp5562_bindings) / sizeof(lp5562_bindings[0]),
    .state_size = sizeof(struct lp5562_state),
    .ops = &lp5562_ops,
};
