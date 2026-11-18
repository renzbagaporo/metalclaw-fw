// SPDX-License-Identifier: Apache-2.0 OR MIT
// TI LP3943 16-LED fun-light driver on I2C: Table 6-1's registers 00h-09h
// as a register map the harness serves from hardware. The two input
// registers read the outputs' levels as the LED selectors set them, and
// take no writes. Data sheet SNVS256E.

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

#define REG_INPUT_1 0x00
#define REG_INPUT_2 0x01
#define REG_PSC0 0x02
#define REG_PWM0 0x03
#define REG_PSC1 0x04
#define REG_PWM1 0x05
#define REG_LS0 0x06
#define REG_LS3 0x09
#define LEDS 16u
// 6.5.4: bit 4 of the register address byte turns auto increment on
// (bits 7-5 are zero, bits 3-0 the register).
#define ADDRESS_AUTO_INCREMENT 0x10
#define ADDRESS_REGISTER 0x0f

enum {
    PROP_SELECTORS,
    PROP_PWM,
};

enum {
    BINDING_I2C,
};

struct lp3943_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    uint8_t pointers[256];
};

static const struct mclw_property_desc lp3943_properties[] = {
    // Each LED's two selector bits, LED0 in bits 1-0: 00 off (high
    // impedance), 01 on, 10 DIM0 (PSC0/PWM0), 11 DIM1.
    { .name = "selectors", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    // PSC0, PWM0, PSC1, PWM1.
    { .name = "pwm", .type = MCLW_TYPE_U32, .count = 4, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
};

static const struct mclw_binding_desc lp3943_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static uint32_t selectors(const struct mclw_regmap *map) {
    uint32_t sel = 0;

    for (uint8_t i = 0; i < 4; i++) {
        sel |= (uint32_t)mclw_regmap_get(map, 0, (uint8_t)(REG_LS0 + i)) << (8u * i);
    }
    return sel;
}

// Table 6-2 and 6-3: an output that sinks (on, 01) reads 0; one left high
// impedance (off, 00) reads 1 through its LED to VDD. A dimmed output
// (10, 11) is read as high here: its PWM phase is not modelled.
static void refresh_inputs(struct mclw_regmap *map) {
    uint32_t sel = selectors(map);
    uint16_t level = 0;

    for (uint32_t led = 0; led < LEDS; led++) {
        if (((sel >> (2u * led)) & 3u) != 1u) {
            level |= (uint16_t)(1u << led);
        }
    }
    mclw_regmap_set(map, 0, REG_INPUT_1, (uint8_t)level, 0);
    mclw_regmap_set(map, 0, REG_INPUT_2, (uint8_t)(level >> 8), 0);
}

static void MCLW_RAM_FUNC(lp3943_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    (void)bank;
    (void)value;
    (void)ctx;
    if (reg >= REG_LS0 && reg <= REG_LS3) {
        refresh_inputs(map);
    }
}

// Table 6-4 to 6-11: PWM0 and PWM1 80h, the rest 00h (every LED off).
static void power_on(struct lp3943_state *s) {
    mclw_regmap_set(&s->map, 0, REG_PSC0, 0x00, MCLW_REG_WRITABLE);
    mclw_regmap_set(&s->map, 0, REG_PWM0, 0x80, MCLW_REG_WRITABLE);
    mclw_regmap_set(&s->map, 0, REG_PSC1, 0x00, MCLW_REG_WRITABLE);
    mclw_regmap_set(&s->map, 0, REG_PWM1, 0x80, MCLW_REG_WRITABLE);
    for (uint8_t reg = REG_LS0; reg <= REG_LS3; reg++) {
        mclw_regmap_set(&s->map, 0, reg, 0x00, MCLW_REG_WRITABLE);
    }
    refresh_inputs(&s->map);
}

static int lp3943_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct lp3943_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    power_on(s);
    for (size_t p = 0; p < 256; p++) {
        s->pointers[p] = (uint8_t)(p & ADDRESS_REGISTER);
    }
    mclw_regmap_set_pointer_map(&s->map, s->pointers);
    mclw_regmap_set_on_write(&s->map, lp3943_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void lp3943_deinit(struct mclw_instance *inst) {
    struct lp3943_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int lp3943_reset(struct mclw_instance *inst, bool factory) {
    struct lp3943_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    power_on(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int lp3943_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct lp3943_state *s = inst->state;

    switch (property) {
        case PROP_SELECTORS:
            out->v[0] = selectors(&s->map);
            return 0;
        case PROP_PWM:
            for (uint8_t i = 0; i < 4; i++) {
                out->v[i] = mclw_regmap_get(&s->map, 0, (uint8_t)(REG_PSC0 + i));
            }
            return 0;
        default:
            return -EINVAL;
    }
}

static int lp3943_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    (void)inst;
    (void)property;
    (void)in;
    return -EINVAL;
}

static const struct mclw_model_ops lp3943_ops = {
    .init = lp3943_init,
    .deinit = lp3943_deinit,
    .reset = lp3943_reset,
    .get = lp3943_get,
    .set = lp3943_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_lp3943 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "lp3943",
    .family = "lp3943",
    .config = NULL,
    .properties = lp3943_properties,
    .property_count = sizeof(lp3943_properties) / sizeof(lp3943_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = lp3943_bindings,
    .binding_count = sizeof(lp3943_bindings) / sizeof(lp3943_bindings[0]),
    .state_size = sizeof(struct lp3943_state),
    .ops = &lp3943_ops,
};
