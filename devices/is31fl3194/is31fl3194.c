// SPDX-License-Identifier: Apache-2.0 OR MIT
// ISSI (Lumissil) IS31FL3194 3-channel LED driver on I2C: Table 1's
// registers 00h-4Fh as a register map the harness serves from hardware.
// The current level registers take effect when C5h is written to the
// Color Update Register (40h); C5h to the Reset Register (4Fh) restores the
// defaults. Data sheet Rev. C, 03/27/2024.

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

#define REG_PRODUCT_ID 0x00
#define REG_CONFIGURATION 0x01
#define REG_OUTPUT_ENABLE 0x02
#define REG_CURRENT_BAND 0x03
#define REG_OUT1 0x10
#define REG_OUT2 0x21
#define REG_OUT3 0x32
#define REG_COLOR_UPDATE 0x40
#define REG_RESET 0x4f
#define REG_LAST 0x4f
#define PRODUCT_ID 0xce
#define KEY 0xc5

enum {
    PROP_CONFIGURATION,
    PROP_CURRENT_BAND,
    PROP_LEVELS,
    PROP_UPDATES,
};

enum {
    BINDING_I2C,
};

struct is31fl3194_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    // The current levels in effect: OUT1-OUT3 as last latched.
    uint8_t levels[3];
    uint32_t updates;
};

static const struct mclw_property_desc is31fl3194_properties[] = {
    // 01h: SSD (bit 0) 1 for normal operation, RGB mode in bits 2:1.
    { .name = "configuration", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "current_band", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    // OUT1, OUT2, OUT3 as the last color update latched them.
    { .name = "levels", .type = MCLW_TYPE_U32, .count = 3, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    // Color updates (C5h to 40h).
    { .name = "updates", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc is31fl3194_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// Table 1's defaults.
static void defaults(struct is31fl3194_state *s) {
    struct mclw_regmap *map = &s->map;

    for (uint32_t reg = 0; reg <= REG_LAST; reg++) {
        mclw_regmap_set(map, 0, (uint8_t)reg, 0x00, MCLW_REG_WRITABLE);
    }
    mclw_regmap_set(map, 0, REG_PRODUCT_ID, PRODUCT_ID, 0);
    mclw_regmap_set(map, 0, REG_OUTPUT_ENABLE, 0x07, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, REG_CURRENT_BAND, 0x15, MCLW_REG_WRITABLE);
    for (uint8_t p = 0; p < 3; p++) {
        mclw_regmap_set(map, 0, (uint8_t)(0x1c + 0x10 * p), 0x01, MCLW_REG_WRITABLE);
        mclw_regmap_set(map, 0, (uint8_t)(0x1e + 0x10 * p), 0x01, MCLW_REG_WRITABLE);
    }
    // 0Dh-0Fh: no pattern running.
    for (uint8_t reg = 0x0d; reg <= 0x0f; reg++) {
        mclw_regmap_set(map, 0, reg, 0x00, 0);
    }
    for (uint8_t i = 0; i < 3; i++) {
        s->levels[i] = 0;
    }
}

static void MCLW_RAM_FUNC(is31fl3194_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct is31fl3194_state *s = ctx;

    (void)bank;
    if (value != KEY) {
        return;
    }
    if (reg == REG_COLOR_UPDATE) {
        s->levels[0] = mclw_regmap_get(map, 0, REG_OUT1);
        s->levels[1] = mclw_regmap_get(map, 0, REG_OUT2);
        s->levels[2] = mclw_regmap_get(map, 0, REG_OUT3);
        s->updates++;
    } else if (reg == REG_RESET) {
        defaults(s);
    }
}

static int is31fl3194_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct is31fl3194_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    defaults(s);
    s->updates = 0;
    mclw_regmap_set_on_write(&s->map, is31fl3194_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void is31fl3194_deinit(struct mclw_instance *inst) {
    struct is31fl3194_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int is31fl3194_reset(struct mclw_instance *inst, bool factory) {
    struct is31fl3194_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    defaults(s);
    s->updates = 0;
    mclw_hal_critical_exit(critical);
    return 0;
}

static int is31fl3194_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct is31fl3194_state *s = inst->state;

    switch (property) {
        case PROP_CONFIGURATION:
            out->v[0] = mclw_regmap_get(&s->map, 0, REG_CONFIGURATION);
            return 0;
        case PROP_CURRENT_BAND:
            out->v[0] = mclw_regmap_get(&s->map, 0, REG_CURRENT_BAND);
            return 0;
        case PROP_LEVELS:
            for (uint8_t i = 0; i < 3; i++) {
                out->v[i] = s->levels[i];
            }
            return 0;
        case PROP_UPDATES:
            out->v[0] = s->updates;
            return 0;
        default:
            return -EINVAL;
    }
}

static int is31fl3194_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    (void)inst;
    (void)property;
    (void)in;
    return -EINVAL;
}

static const struct mclw_model_ops is31fl3194_ops = {
    .init = is31fl3194_init,
    .deinit = is31fl3194_deinit,
    .reset = is31fl3194_reset,
    .get = is31fl3194_get,
    .set = is31fl3194_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_is31fl3194 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "is31fl3194",
    .family = "is31fl3194",
    .config = NULL,
    .properties = is31fl3194_properties,
    .property_count = sizeof(is31fl3194_properties) / sizeof(is31fl3194_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = is31fl3194_bindings,
    .binding_count = sizeof(is31fl3194_bindings) / sizeof(is31fl3194_bindings[0]),
    .state_size = sizeof(struct is31fl3194_state),
    .ops = &is31fl3194_ops,
};
