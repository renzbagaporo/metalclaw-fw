// SPDX-License-Identifier: Apache-2.0 OR MIT
// ISSI (Lumissil) IS31FL3216A 16-channel LED driver on I2C: Table 2's
// registers as a register map the harness serves from hardware. PWM data
// (10h-1Fh, OUT16 first) waits in its registers until a write to the
// Update Register (B0h) loads it into the outputs. Data sheet Rev. B,
// 02/24/2025.

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
#define REG_LED_CTL_1 0x01
#define REG_LED_CTL_2 0x02
#define REG_PWM_FIRST 0x10
#define REG_PWM_LAST 0x1f
#define REG_UPDATE 0xb0
#define REG_LAST 0xb7
#define CONFIG_SSD 0x80
#define OUTPUTS 16u

enum {
    PROP_ON,
    PROP_OUT_LOW,
    PROP_OUT_HIGH,
    PROP_UPDATES,
};

enum {
    BINDING_I2C,
};

struct is31fl3216a_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    // OUT1-OUT16's duty as last loaded by an update.
    volatile uint8_t outputs[OUTPUTS];
    volatile uint32_t updates;
};

// A property holds at most MCLW_VALUE_MAX_COUNT values: OUT1-OUT8 in one,
// OUT9-OUT16 in the other.
static const struct mclw_property_desc is31fl3216a_properties[] = {
    { .name = "on", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "out_low", .type = MCLW_TYPE_U32, .count = 8, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "out_high", .type = MCLW_TYPE_U32, .count = 8, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "updates", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc is31fl3216a_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// Table 2's defaults: Configuration 80h (software shutdown), both LED
// Control Registers FFh, the rest 00h.
static void power_on(struct is31fl3216a_state *s) {
    for (uint16_t reg = 0; reg <= REG_LAST; reg++) {
        uint8_t value = 0x00;

        if (reg == REG_CONFIG) {
            value = CONFIG_SSD;
        } else if (reg == REG_LED_CTL_1 || reg == REG_LED_CTL_2) {
            value = 0xff;
        }
        mclw_regmap_set(&s->map, 0, (uint8_t)reg, value, MCLW_REG_WRITABLE);
    }
    for (size_t i = 0; i < OUTPUTS; i++) {
        s->outputs[i] = 0;
    }
}

// B0h Update Register: any write loads the PWM Registers into the
// outputs. 10h is OUT16, 1Fh OUT1 (Table 13).
static void MCLW_RAM_FUNC(is31fl3216a_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx) {
    struct is31fl3216a_state *s = ctx;

    (void)bank;
    (void)value;
    if (reg != REG_UPDATE) {
        return;
    }
    for (size_t i = 0; i < OUTPUTS; i++) {
        s->outputs[i] = mclw_regmap_get(map, 0, (uint8_t)(REG_PWM_LAST - i));
    }
    s->updates = s->updates + 1u;
}

static int is31fl3216a_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct is31fl3216a_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->updates = 0;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    power_on(s);
    mclw_regmap_set_on_write(&s->map, is31fl3216a_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void is31fl3216a_deinit(struct mclw_instance *inst) {
    struct is31fl3216a_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int is31fl3216a_reset(struct mclw_instance *inst, bool factory) {
    struct is31fl3216a_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    power_on(s);
    mclw_hal_critical_exit(critical);
    s->updates = 0;
    return 0;
}

static int is31fl3216a_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct is31fl3216a_state *s = inst->state;

    switch (property) {
        case PROP_ON:
            out->v[0] = (mclw_regmap_get(&s->map, 0, REG_CONFIG) & CONFIG_SSD) == 0;
            return 0;
        case PROP_OUT_LOW:
            for (size_t i = 0; i < 8; i++) {
                out->v[i] = s->outputs[i];
            }
            return 0;
        case PROP_OUT_HIGH:
            for (size_t i = 0; i < 8; i++) {
                out->v[i] = s->outputs[8 + i];
            }
            return 0;
        case PROP_UPDATES:
            out->v[0] = s->updates;
            return 0;
        default:
            return -EINVAL;
    }
}

static int is31fl3216a_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    (void)inst;
    (void)property;
    (void)in;
    return -EINVAL;
}

static const struct mclw_model_ops is31fl3216a_ops = {
    .init = is31fl3216a_init,
    .deinit = is31fl3216a_deinit,
    .reset = is31fl3216a_reset,
    .get = is31fl3216a_get,
    .set = is31fl3216a_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_is31fl3216a = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "is31fl3216a",
    .family = "is31fl3216a",
    .config = NULL,
    .properties = is31fl3216a_properties,
    .property_count = sizeof(is31fl3216a_properties) / sizeof(is31fl3216a_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = is31fl3216a_bindings,
    .binding_count = sizeof(is31fl3216a_bindings) / sizeof(is31fl3216a_bindings[0]),
    .state_size = sizeof(struct is31fl3216a_state),
    .ops = &is31fl3216a_ops,
};
