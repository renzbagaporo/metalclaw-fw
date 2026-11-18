// SPDX-License-Identifier: Apache-2.0 OR MIT
// Semtech SX1509B 16-I/O expander with LED driver on I2C: Table 13's
// configuration registers 00h-7Fh as a register map the harness serves
// from hardware, with the RegReset software reset. The register map is
// the SX1507/SX1508/SX1509 data sheet's (Rev 1, 30 Oct. 2009); Zephyr's
// sx1509b driver uses the same addresses.

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

#define REG_DIR_B 0x0e
#define REG_DATA_B 0x10
#define REG_INTERRUPT_MASK_B 0x12
#define REG_LED_DRIVER_ENABLE_B 0x20
#define REG_KEY_DATA_1 0x27
#define REG_RESET 0x7d
#define REG_LAST 0x7f
#define RESET_MAGIC_0 0x12
#define RESET_MAGIC_1 0x34
#define IOS 16u

enum {
    PROP_DIRECTION,
    PROP_DATA,
    PROP_LED_DRIVER,
    PROP_INTENSITY_LOW,
    PROP_INTENSITY_HIGH,
};

enum {
    BINDING_I2C,
};

struct sx1509b_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    volatile bool reset_armed;
};

// A property holds at most MCLW_VALUE_MAX_COUNT values: I/O 0-7's
// RegIOn in one, I/O 8-15's in the other.
static const struct mclw_property_desc sx1509b_properties[] = {
    { .name = "direction", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 0xffff },
    { .name = "data", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 0xffff },
    { .name = "led_driver", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 0xffff },
    { .name = "intensity_low", .type = MCLW_TYPE_U32, .count = 8, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "intensity_high", .type = MCLW_TYPE_U32, .count = 8, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
};

static const struct mclw_binding_desc sx1509b_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// Table 13: each I/O's RegIOn (ON intensity). I/Os 0-3 and 8-11 have
// TOn, IOn and Off; 4-7 and 12-15 add TRise and TFall.
static uint8_t ion_register(uint32_t io) {
    static const uint8_t ion[IOS] = {
        0x2a, 0x2d, 0x30, 0x33, 0x36, 0x3b, 0x40, 0x45,
        0x4a, 0x4d, 0x50, 0x53, 0x56, 0x5b, 0x60, 0x65,
    };
    return ion[io];
}

// Table 13's defaults: RegDir, RegData, RegInterruptMask and RegKeyData
// FFh in both banks, every RegIOn FFh, the rest 00h.
static void load_defaults(struct mclw_regmap *map) {
    for (uint16_t reg = 0; reg <= REG_LAST; reg++) {
        mclw_regmap_set(map, 0, (uint8_t)reg, 0x00, MCLW_REG_WRITABLE);
    }
    for (uint8_t i = 0; i < 2; i++) {
        mclw_regmap_set(map, 0, (uint8_t)(REG_DIR_B + i), 0xff, MCLW_REG_WRITABLE);
        mclw_regmap_set(map, 0, (uint8_t)(REG_DATA_B + i), 0xff, MCLW_REG_WRITABLE);
        mclw_regmap_set(map, 0, (uint8_t)(REG_INTERRUPT_MASK_B + i), 0xff, MCLW_REG_WRITABLE);
        mclw_regmap_set(map, 0, (uint8_t)(REG_KEY_DATA_1 + i), 0xff, MCLW_REG_WRITABLE);
    }
    for (uint32_t io = 0; io < IOS; io++) {
        mclw_regmap_set(map, 0, ion_register(io), 0xff, MCLW_REG_WRITABLE);
    }
}

// 4.4.2: 12h then 34h written to RegReset resets every register.
static void MCLW_RAM_FUNC(sx1509b_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct sx1509b_state *s = ctx;

    (void)bank;
    if (reg != REG_RESET) {
        return;
    }
    if (s->reset_armed && value == RESET_MAGIC_1) {
        load_defaults(map);
        s->reset_armed = false;
        return;
    }
    s->reset_armed = value == RESET_MAGIC_0;
    mclw_regmap_set(map, 0, REG_RESET, 0x00, MCLW_REG_WRITABLE);
}

static uint32_t bank_pair(const struct sx1509b_state *s, uint8_t reg_b) {
    return ((uint32_t)mclw_regmap_get(&s->map, 0, reg_b) << 8) | mclw_regmap_get(&s->map, 0, (uint8_t)(reg_b + 1));
}

static int sx1509b_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct sx1509b_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->reset_armed = false;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    load_defaults(&s->map);
    mclw_regmap_set_on_write(&s->map, sx1509b_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void sx1509b_deinit(struct mclw_instance *inst) {
    struct sx1509b_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int sx1509b_reset(struct mclw_instance *inst, bool factory) {
    struct sx1509b_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    load_defaults(&s->map);
    s->reset_armed = false;
    mclw_hal_critical_exit(critical);
    return 0;
}

static int sx1509b_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct sx1509b_state *s = inst->state;

    switch (property) {
        case PROP_DIRECTION:
            out->v[0] = bank_pair(s, REG_DIR_B);
            return 0;
        case PROP_DATA:
            out->v[0] = bank_pair(s, REG_DATA_B);
            return 0;
        case PROP_LED_DRIVER:
            out->v[0] = bank_pair(s, REG_LED_DRIVER_ENABLE_B);
            return 0;
        case PROP_INTENSITY_LOW:
        case PROP_INTENSITY_HIGH: {
            uint32_t first = property == PROP_INTENSITY_LOW ? 0u : 8u;
            for (uint32_t i = 0; i < 8; i++) {
                out->v[i] = mclw_regmap_get(&s->map, 0, ion_register(first + i));
            }
            return 0;
        }
        default:
            return -EINVAL;
    }
}

static int sx1509b_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    (void)inst;
    (void)property;
    (void)in;
    return -EINVAL;
}

static const struct mclw_model_ops sx1509b_ops = {
    .init = sx1509b_init,
    .deinit = sx1509b_deinit,
    .reset = sx1509b_reset,
    .get = sx1509b_get,
    .set = sx1509b_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_sx1509b = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "sx1509b",
    .family = "sx1509b",
    .config = NULL,
    .properties = sx1509b_properties,
    .property_count = sizeof(sx1509b_properties) / sizeof(sx1509b_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = sx1509b_bindings,
    .binding_count = sizeof(sx1509b_bindings) / sizeof(sx1509b_bindings[0]),
    .state_size = sizeof(struct sx1509b_state),
    .ops = &sx1509b_ops,
};
