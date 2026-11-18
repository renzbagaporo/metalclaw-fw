// SPDX-License-Identifier: Apache-2.0 OR MIT
// TI LP5012 12-channel RGB LED driver on I2C: Table 8-4's registers
// 00h-17h as a register map the harness serves from hardware, with the
// RESET register's software reset. Data sheet SLVSEH2B (LP5009, LP5012).

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

#define REG_DEVICE_CONFIG0 0x00
#define REG_DEVICE_CONFIG1 0x01
#define REG_LED0_BRIGHTNESS 0x07
#define REG_OUT0_COLOR 0x0b
#define REG_RESET 0x17
#define CONFIG0_CHIP_EN 0x40
#define CONFIG1_GLOBAL_OFF 0x01
#define RESET_SW 0xff
#define MODULES 4u

// Table 8-4's defaults, 00h-16h; RESET (17h) is write-only and reads 00h.
static const uint8_t defaults[REG_RESET] = {
    0x00, 0x3c, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// A property holds at most MCLW_VALUE_MAX_COUNT values, so the twelve
// OUTx_COLOR registers are one property per RGB module.
enum {
    PROP_ENABLED,
    PROP_BRIGHTNESS,
    PROP_WRITES,
    PROP_LED0,
};

enum {
    BINDING_I2C,
};

struct lp50xx_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    volatile uint32_t writes;
};

static const struct mclw_property_desc lp50xx_properties[] = {
    { .name = "enabled", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "brightness", .type = MCLW_TYPE_U32, .count = MODULES, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "writes", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "led0", .type = MCLW_TYPE_U32, .count = 3, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "led1", .type = MCLW_TYPE_U32, .count = 3, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "led2", .type = MCLW_TYPE_U32, .count = 3, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "led3", .type = MCLW_TYPE_U32, .count = 3, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
};

static const struct mclw_binding_desc lp50xx_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static void load_defaults(struct mclw_regmap *map) {
    for (uint8_t reg = 0; reg < REG_RESET; reg++) {
        mclw_regmap_set(map, 0, reg, defaults[reg], MCLW_REG_WRITABLE);
    }
    mclw_regmap_set(map, 0, REG_RESET, 0x00, MCLW_REG_WRITABLE);
}

// 8.6.24: writing FFh to RESET resets every register to its default.
static void MCLW_RAM_FUNC(lp50xx_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct lp50xx_state *s = ctx;

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

static int lp50xx_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct lp50xx_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->writes = 0;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    load_defaults(&s->map);
    mclw_regmap_set_on_write(&s->map, lp50xx_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void lp50xx_deinit(struct mclw_instance *inst) {
    struct lp50xx_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int lp50xx_reset(struct mclw_instance *inst, bool factory) {
    struct lp50xx_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    load_defaults(&s->map);
    mclw_hal_critical_exit(critical);
    s->writes = 0;
    return 0;
}

static int lp50xx_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct lp50xx_state *s = inst->state;

    switch (property) {
        case PROP_ENABLED:
            out->v[0] = (mclw_regmap_get(&s->map, 0, REG_DEVICE_CONFIG0) & CONFIG0_CHIP_EN) != 0;
            return 0;
        case PROP_BRIGHTNESS:
            for (size_t i = 0; i < MODULES; i++) {
                out->v[i] = mclw_regmap_get(&s->map, 0, (uint8_t)(REG_LED0_BRIGHTNESS + i));
            }
            return 0;
        case PROP_WRITES:
            out->v[0] = s->writes;
            return 0;
        default:
            if (property >= PROP_LED0 && property < PROP_LED0 + MODULES) {
                size_t base = (property - PROP_LED0) * 3u;
                for (size_t i = 0; i < 3; i++) {
                    out->v[i] = mclw_regmap_get(&s->map, 0, (uint8_t)(REG_OUT0_COLOR + base + i));
                }
                return 0;
            }
            return -EINVAL;
    }
}

static int lp50xx_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    (void)inst;
    (void)property;
    (void)in;
    return -EINVAL;
}

static const struct mclw_model_ops lp50xx_ops = {
    .init = lp50xx_init,
    .deinit = lp50xx_deinit,
    .reset = lp50xx_reset,
    .get = lp50xx_get,
    .set = lp50xx_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_lp5012 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "lp5012",
    .family = "lp50xx",
    .config = NULL,
    .properties = lp50xx_properties,
    .property_count = sizeof(lp50xx_properties) / sizeof(lp50xx_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = lp50xx_bindings,
    .binding_count = sizeof(lp50xx_bindings) / sizeof(lp50xx_bindings[0]),
    .state_size = sizeof(struct lp50xx_state),
    .ops = &lp50xx_ops,
};
