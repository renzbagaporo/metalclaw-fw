// SPDX-License-Identifier: Apache-2.0 OR MIT
// NXP PCA9633 4-bit Fm+ I2C-bus LED driver, a register map the harness
// serves from hardware. Section and table numbers are the data sheet's,
// Rev. 05, 25 July 2008.

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

// Table 7: 00h..0Ch; the control register's D[3:0] names one, its three
// high bits the Auto-Increment option (7.2, table 6).
#define REG_MODE1 0x00u
#define REG_MODE2 0x01u
#define REG_PWM0 0x02u
#define REG_GRPPWM 0x06u
#define REG_GRPFREQ 0x07u
#define REG_LEDOUT 0x08u
#define REG_SUBADR1 0x09u
#define REG_ALLCALLADR 0x0cu
#define REGISTERS 13u
#define LEDS 4u
#define MODE1_SLEEP 0x10u
#define MODE2_DMBLNK 0x20u

enum {
    PROP_BRIGHTNESS,
    PROP_LEDOUT,
    PROP_SLEEP,
    PROP_WRITES,
};

enum {
    BINDING_I2C,
};

struct pca9633_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    uint8_t pointers[MCLW_REGMAP_BANK_SIZE];
    volatile uint32_t writes;
};

static const struct mclw_property_desc pca9633_properties[] = {
    { .name = "brightness", .type = MCLW_TYPE_U32, .count = LEDS, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "ledout", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "sleep", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "writes", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc pca9633_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static uint8_t get(const struct pca9633_state *s, uint8_t reg) {
    return mclw_regmap_get(&s->map, 0, reg);
}

// The reset values (tables 8-15): MODE1 91h (Auto-Increment on, SLEEP,
// ALLCALL), MODE2 05h, PWMx 0, GRPPWM FFh, GRPFREQ 0, LEDOUT 0 (all off),
// SUBADR1-3 E2h/E4h/E8h, ALLCALLADR E0h.
static void reset_registers(struct pca9633_state *s) {
    static const uint8_t reset[REGISTERS] = {
        0x91, 0x05, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xe2, 0xe4, 0xe8, 0xe0,
    };

    for (uint8_t reg = 0; reg < REGISTERS; reg++) {
        // MODE1's AI2..AI0 read the control register's option; the model keeps them as at reset.
        mclw_regmap_set(&s->map, 0, reg, reset[reg], MCLW_REG_WRITABLE);
    }
    s->writes = 0;
}

static void MCLW_RAM_FUNC(pca9633_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct pca9633_state *s = ctx;

    (void)bank;
    if (reg == REG_MODE1) {
        // AI2..AI0 are read-only (table 8).
        mclw_regmap_set(map, 0, reg, (uint8_t)((get(s, REG_MODE1) & 0xe0u) | (value & 0x1fu)), MCLW_REG_WRITABLE);
    } else if (reg == REG_MODE2) {
        // Bits 7:6 are reserved, read 0 (table 9).
        mclw_regmap_set(map, 0, reg, (uint8_t)(value & 0x3fu), MCLW_REG_WRITABLE);
    }
    s->writes = s->writes + 1u;
}

// What LEDn shows (7.3.6, table 13): off, fully on, its PWMx duty, or that
// dimmed by GRPPWM in dimming mode (DMBLNK 0; blinking is not modelled and
// shows PWMx). Nothing shows while the oscillator sleeps (table 8, note 2).
static uint32_t brightness(const struct pca9633_state *s, uint8_t led) {
    uint8_t ldr = (uint8_t)((get(s, REG_LEDOUT) >> (2u * led)) & 3u);
    uint32_t pwm = get(s, (uint8_t)(REG_PWM0 + led));

    if ((get(s, REG_MODE1) & MODE1_SLEEP) != 0 && ldr >= 2u) {
        return 0;
    }
    switch (ldr) {
        case 0:
            return 0;
        case 1:
            return 255;
        case 2:
            return pwm;
        default:
            return (get(s, REG_MODE2) & MODE2_DMBLNK) != 0 ? pwm : pwm * get(s, REG_GRPPWM) / 255u;
    }
}

static int pca9633_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct pca9633_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    // D[3:0] picks the register; 0Dh..0Fh are reserved (table 7, note 1).
    for (size_t p = 0; p < MCLW_REGMAP_BANK_SIZE; p++) {
        s->pointers[p] = (p & 0x0fu) < REGISTERS ? (uint8_t)(p & 0x0fu) : REGISTERS;
    }
    mclw_regmap_set_pointer_map(&s->map, s->pointers);
    mclw_regmap_set_on_write(&s->map, pca9633_on_write, s);
    uint32_t critical = mclw_hal_critical_enter();
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void pca9633_deinit(struct mclw_instance *inst) {
    struct pca9633_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int pca9633_reset(struct mclw_instance *inst, bool factory) {
    struct pca9633_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int pca9633_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct pca9633_state *s = inst->state;

    switch (property) {
        case PROP_BRIGHTNESS:
            for (uint8_t led = 0; led < LEDS; led++) {
                out->v[led] = brightness(s, led);
            }
            return 0;
        case PROP_LEDOUT:
            out->v[0] = get(s, REG_LEDOUT);
            return 0;
        case PROP_SLEEP:
            out->v[0] = (get(s, REG_MODE1) & MODE1_SLEEP) != 0;
            return 0;
        case PROP_WRITES:
            out->v[0] = s->writes;
            return 0;
        default:
            return -EINVAL;
    }
}

static int pca9633_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    (void)inst;
    (void)property;
    (void)in;
    return -EINVAL;
}

static const struct mclw_model_ops pca9633_ops = {
    .init = pca9633_init,
    .deinit = pca9633_deinit,
    .reset = pca9633_reset,
    .get = pca9633_get,
    .set = pca9633_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_pca9633 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "pca9633",
    .family = "pca9633",
    .config = NULL,
    .properties = pca9633_properties,
    .property_count = sizeof(pca9633_properties) / sizeof(pca9633_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = pca9633_bindings,
    .binding_count = sizeof(pca9633_bindings) / sizeof(pca9633_bindings[0]),
    .state_size = sizeof(struct pca9633_state),
    .ops = &pca9633_ops,
};
