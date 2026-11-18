// SPDX-License-Identifier: Apache-2.0 OR MIT
// TI LP5569 9-channel LED driver on I2C: the register map of 8.6.1, 00h-4Fh,
// at its reset values, a register map the harness serves from hardware,
// with the RESET register's software reset. Data sheet SNVSAP8A.

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

#define REG_CONFIG 0x00
#define REG_LED0_PWM 0x16
#define REG_LED0_CURRENT 0x22
#define REG_MISC 0x2f
#define REG_MISC2 0x33
#define REG_ENGINE_STATUS 0x3c
#define REG_IO_CONTROL 0x3d
#define REG_RESET 0x3f
#define REG_LAST 0x4f
#define CONFIG_CHIP_EN 0x40
#define RESET_SW 0xff
#define LEDS 9u

enum {
    PROP_ENABLED,
    PROP_PWM_LOW,
    PROP_PWM_HIGH,
    PROP_WRITES,
};

enum {
    BINDING_I2C,
};

struct lp5569_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    volatile uint32_t writes;
};

// A property holds at most MCLW_VALUE_MAX_COUNT values: LED0-LED4's PWM
// in one, LED5-LED8's in the other.
static const struct mclw_property_desc lp5569_properties[] = {
    { .name = "enabled", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "pwm_low", .type = MCLW_TYPE_U32, .count = 5, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "pwm_high", .type = MCLW_TYPE_U32, .count = 4, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "writes", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc lp5569_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// 8.6.1's reset values: LEDx_CURRENT AFh, MISC 40h (EN_AUTO_INCR), MISC2
// 02h, ENGINE_STATUS 80h (mask_busy), IO_CONTROL 02h, the rest 00h. RESET
// is write-only and reads 00h.
static void load_defaults(struct mclw_regmap *map) {
    for (uint8_t reg = 0; reg <= REG_LAST; reg++) {
        uint8_t value = 0x00;

        if (reg >= REG_LED0_CURRENT && reg < REG_LED0_CURRENT + LEDS) {
            value = 0xaf;
        } else if (reg == REG_MISC) {
            value = 0x40;
        } else if (reg == REG_MISC2 || reg == REG_IO_CONTROL) {
            value = 0x02;
        } else if (reg == REG_ENGINE_STATUS) {
            value = 0x80;
        }
        mclw_regmap_set(map, 0, reg, value, MCLW_REG_WRITABLE);
    }
}

// Table 54: FFh in RESET resets the device's registers to their defaults.
static void MCLW_RAM_FUNC(lp5569_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct lp5569_state *s = ctx;

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

static int lp5569_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct lp5569_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->writes = 0;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    load_defaults(&s->map);
    mclw_regmap_set_on_write(&s->map, lp5569_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void lp5569_deinit(struct mclw_instance *inst) {
    struct lp5569_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int lp5569_reset(struct mclw_instance *inst, bool factory) {
    struct lp5569_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    load_defaults(&s->map);
    mclw_hal_critical_exit(critical);
    s->writes = 0;
    return 0;
}

static int lp5569_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct lp5569_state *s = inst->state;

    switch (property) {
        case PROP_ENABLED:
            out->v[0] = (mclw_regmap_get(&s->map, 0, REG_CONFIG) & CONFIG_CHIP_EN) != 0;
            return 0;
        case PROP_PWM_LOW:
            for (size_t i = 0; i < 5; i++) {
                out->v[i] = mclw_regmap_get(&s->map, 0, (uint8_t)(REG_LED0_PWM + i));
            }
            return 0;
        case PROP_PWM_HIGH:
            for (size_t i = 0; i < 4; i++) {
                out->v[i] = mclw_regmap_get(&s->map, 0, (uint8_t)(REG_LED0_PWM + 5u + i));
            }
            return 0;
        case PROP_WRITES:
            out->v[0] = s->writes;
            return 0;
        default:
            return -EINVAL;
    }
}

static int lp5569_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    (void)inst;
    (void)property;
    (void)in;
    return -EINVAL;
}

static const struct mclw_model_ops lp5569_ops = {
    .init = lp5569_init,
    .deinit = lp5569_deinit,
    .reset = lp5569_reset,
    .get = lp5569_get,
    .set = lp5569_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_lp5569 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "lp5569",
    .family = "lp5569",
    .config = NULL,
    .properties = lp5569_properties,
    .property_count = sizeof(lp5569_properties) / sizeof(lp5569_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = lp5569_bindings,
    .binding_count = sizeof(lp5569_bindings) / sizeof(lp5569_bindings[0]),
    .state_size = sizeof(struct lp5569_state),
    .ops = &lp5569_ops,
};
