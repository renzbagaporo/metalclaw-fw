// SPDX-License-Identifier: Apache-2.0 OR MIT
// ISSI (Lumissil) IS31FL3733 12x16 dot-matrix LED driver on I2C: its four
// register pages as a register map with four banks the harness serves from
// hardware. FDh picks the page, but only once after C5h is written to FEh
// (Table 3), which an on_write hook enforces. Data sheet Rev. G,
// 10/29/2024.

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

#define PAGES 4u
#define PAGE_LED 0u
#define PAGE_PWM 1u
#define PAGE_ABM 2u
#define PAGE_FUNCTION 3u

#define REG_COMMAND 0xfd
#define REG_LOCK 0xfe
#define REG_INT_MASK 0xf0
#define REG_INT_STATUS 0xf1
#define LOCK_UNLOCK 0xc5

// Page 0: LED on/off 00h-17h; open 18h-2Fh and short 30h-47h read 0, no
// LED being open or shorted. Pages 1 and 2: one register per dot, 00h-BFh.
// Page 3: the function registers 00h-10h; 11h resets on a read.
#define LED_ONOFF_COUNT 0x18u
#define DOTS 0xc0u
#define FUNCTION_LAST 0x10u
#define REG_CONFIGURATION 0x00
#define REG_GCC 0x01
#define CONFIGURATION_SSD 0x01u

enum {
    PROP_PAGE,
    PROP_CONFIGURATION,
    PROP_GCC,
    PROP_LEDS_ON,
    PROP_PWM_LIT,
    PROP_PWM_MAX,
};

enum {
    BINDING_I2C,
};

struct is31fl3733_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    bool unlocked;
};

static const struct mclw_property_desc is31fl3733_properties[] = {
    // The page FDh points to.
    { .name = "page", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 3 },
    // Page 3's 00h: SSD (bit 0) 1 for normal operation.
    { .name = "configuration", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "gcc", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    // Dots switched on in page 0, of 192.
    { .name = "leds_on", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 192 },
    // Dots whose PWM (page 1) is not zero, and the highest PWM.
    { .name = "pwm_lit", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 192 },
    { .name = "pwm_max", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
};

static const struct mclw_binding_desc is31fl3733_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// FEh and F0h are in every page; F1h reads no interrupt.
static void set_global(struct mclw_regmap *map, uint8_t reg, uint8_t value, uint8_t flags) {
    for (uint8_t page = 0; page < PAGES; page++) {
        mclw_regmap_set(map, page, reg, value, flags);
    }
}

// Table 3: C5h in FEh lets FDh change once, after which FEh is 00h again.
static void MCLW_RAM_FUNC(is31fl3733_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct is31fl3733_state *s = ctx;

    if (reg == REG_COMMAND) {
        if (!s->unlocked) {
            map->active_bank = bank;
        }
        s->unlocked = false;
        set_global(map, REG_LOCK, 0x00, MCLW_REG_WRITABLE);
    } else if (reg == REG_LOCK) {
        s->unlocked = value == LOCK_UNLOCK;
        set_global(map, REG_LOCK, value, MCLW_REG_WRITABLE);
    } else if (reg == REG_INT_MASK) {
        set_global(map, REG_INT_MASK, value, MCLW_REG_WRITABLE);
    }
}

// The POR state: every register 00h, page 0 selected, FDh locked.
static void power_on(struct is31fl3733_state *s) {
    struct mclw_regmap *map = &s->map;

    for (uint8_t page = 0; page < PAGES; page++) {
        for (uint32_t reg = 0; reg < 0x100u; reg++) {
            mclw_regmap_set(map, page, (uint8_t)reg, 0x00, 0);
        }
    }
    for (uint8_t reg = 0; reg < LED_ONOFF_COUNT; reg++) {
        mclw_regmap_set(map, PAGE_LED, reg, 0x00, MCLW_REG_WRITABLE);
    }
    for (uint32_t reg = 0; reg < DOTS; reg++) {
        mclw_regmap_set(map, PAGE_PWM, (uint8_t)reg, 0x00, MCLW_REG_WRITABLE);
        mclw_regmap_set(map, PAGE_ABM, (uint8_t)reg, 0x00, MCLW_REG_WRITABLE);
    }
    for (uint8_t reg = 0; reg <= FUNCTION_LAST; reg++) {
        mclw_regmap_set(map, PAGE_FUNCTION, reg, 0x00, MCLW_REG_WRITABLE);
    }
    set_global(map, REG_LOCK, 0x00, MCLW_REG_WRITABLE);
    set_global(map, REG_INT_MASK, 0x00, MCLW_REG_WRITABLE);
    map->active_bank = PAGE_LED;
    s->unlocked = false;
}

static int is31fl3733_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct is31fl3733_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    mclw_regmap_init(&s->map, PAGES, REG_COMMAND, 0x00);
    power_on(s);
    mclw_regmap_set_on_write(&s->map, is31fl3733_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void is31fl3733_deinit(struct mclw_instance *inst) {
    struct is31fl3733_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int is31fl3733_reset(struct mclw_instance *inst, bool factory) {
    struct is31fl3733_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    power_on(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int is31fl3733_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct is31fl3733_state *s = inst->state;
    uint32_t count = 0;
    uint32_t max = 0;

    switch (property) {
        case PROP_PAGE:
            out->v[0] = s->map.active_bank;
            return 0;
        case PROP_CONFIGURATION:
            out->v[0] = mclw_regmap_get(&s->map, PAGE_FUNCTION, REG_CONFIGURATION);
            return 0;
        case PROP_GCC:
            out->v[0] = mclw_regmap_get(&s->map, PAGE_FUNCTION, REG_GCC);
            return 0;
        case PROP_LEDS_ON:
            for (uint8_t reg = 0; reg < LED_ONOFF_COUNT; reg++) {
                count += (uint32_t)__builtin_popcount(mclw_regmap_get(&s->map, PAGE_LED, reg));
            }
            out->v[0] = count;
            return 0;
        case PROP_PWM_LIT:
        case PROP_PWM_MAX:
            for (uint32_t reg = 0; reg < DOTS; reg++) {
                uint8_t v = mclw_regmap_get(&s->map, PAGE_PWM, (uint8_t)reg);

                count += v != 0u;
                max = v > max ? v : max;
            }
            out->v[0] = property == PROP_PWM_LIT ? count : max;
            return 0;
        default:
            return -EINVAL;
    }
}

static int is31fl3733_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    (void)inst;
    (void)property;
    (void)in;
    return -EINVAL;
}

static const struct mclw_model_ops is31fl3733_ops = {
    .init = is31fl3733_init,
    .deinit = is31fl3733_deinit,
    .reset = is31fl3733_reset,
    .get = is31fl3733_get,
    .set = is31fl3733_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_is31fl3733 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "is31fl3733",
    .family = "is31fl3733",
    .config = NULL,
    .properties = is31fl3733_properties,
    .property_count = sizeof(is31fl3733_properties) / sizeof(is31fl3733_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = is31fl3733_bindings,
    .binding_count = sizeof(is31fl3733_bindings) / sizeof(is31fl3733_bindings[0]),
    .state_size = sizeof(struct is31fl3733_state),
    .ops = &is31fl3733_ops,
};
