// SPDX-License-Identifier: Apache-2.0 OR MIT
// FocalTech FT5336 capacitive touch panel controller on I2C: the work-mode
// register map of FocalTech's FT5x06/FT5x16 CTPM application notes (V1.1,
// 15 Apr 2011, and V1.0, 28 May 2012), the same map Zephyr's ft5336 driver
// reads, as a register map the harness serves from hardware. One touch
// point, which the host places and lifts; /INT low while it is down.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

// Work Mode Register Map (FT5x06/5x16 note, 2.1).
#define REG_DEVICE_MODE 0x00
#define REG_GEST_ID 0x01
#define REG_TD_STATUS 0x02
#define REG_TOUCH1_XH 0x03
#define TOUCH_STRIDE 6u
#define TOUCH_POINTS 10u
#define REG_ID_G_THGROUP 0x80
#define REG_ID_G_CIPHER 0xa3
#define REG_ID_G_MODE 0xa4
#define REG_ID_G_PMODE 0xa5
#define REG_ID_G_FIRMID 0xa6
#define REG_ID_G_STATE 0xa7
#define REG_ID_G_VENDOR 0xa8

// 2.1.4: event flag in bits 7-6 of TOUCHn_XH; 2.1.6: touch ID in bits
// 7-4 of TOUCHn_YH, 0Fh for no point (as Zephyr's driver reads it).
#define EVENT_PUT_DOWN 0x00
#define EVENT_CONTACT 0x80
#define TOUCH_ID_NONE 0x0f
// The FT5x16 note's ID_G_CIPHER (3.1.44).
#define CIPHER 0x0a

enum {
    PROP_POSITION,
    PROP_PRESSED,
};

enum {
    BINDING_I2C,
    BINDING_INT,
};

struct ft5336_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    struct mclw_gpio *int_pin;
    int32_t x;
    int32_t y;
    bool pressed;
    bool reported_down;
};

static const struct mclw_property_desc ft5336_properties[] = {
    { .name = "position", .type = MCLW_TYPE_I32, .count = 2, .access = MCLW_ACCESS_RW, .unit = "px",
      .min = 0, .max = 4095 },
    { .name = "pressed", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = 1 },
};

static const struct mclw_binding_desc ft5336_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
    {
        .name = "int",
        .kind = MCLW_BINDING_GPIO_OUT,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static void put(struct ft5336_state *s, uint8_t reg, uint8_t value, uint8_t flags) {
    mclw_regmap_set(&s->map, 0, reg, value, flags);
}

// The touch registers for the point as the host set it: TD_STATUS 1 and
// point 1 at (x, y), ID 0, put down on the first read after it went down
// and contact after; or TD_STATUS 0 and no point.
static void publish(struct ft5336_state *s) {
    uint32_t critical = mclw_hal_critical_enter();
    uint8_t event = s->reported_down ? EVENT_CONTACT : EVENT_PUT_DOWN;

    if (s->pressed) {
        put(s, REG_TD_STATUS, 1, 0);
        put(s, REG_TOUCH1_XH, (uint8_t)(event | ((s->x >> 8) & 0x0f)), 0);
        put(s, REG_TOUCH1_XH + 1, (uint8_t)s->x, 0);
        put(s, REG_TOUCH1_XH + 2, (uint8_t)((s->y >> 8) & 0x0f), 0);
        put(s, REG_TOUCH1_XH + 3, (uint8_t)s->y, 0);
    } else {
        put(s, REG_TD_STATUS, 0, 0);
        put(s, REG_TOUCH1_XH, 0xff, 0);
        put(s, REG_TOUCH1_XH + 1, 0xff, 0);
        put(s, REG_TOUCH1_XH + 2, 0xff, 0);
        put(s, REG_TOUCH1_XH + 3, 0xff, 0);
    }
    mclw_hal_critical_exit(critical);
    if (s->int_pin != NULL) {
        mclw_hal_gpio_write(s->int_pin, !s->pressed);
    }
}

static void power_on(struct ft5336_state *s) {
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    for (uint32_t p = 1; p < TOUCH_POINTS; p++) {
        uint8_t base = (uint8_t)(REG_TOUCH1_XH + p * TOUCH_STRIDE);
        put(s, base, 0xff, 0);
        put(s, (uint8_t)(base + 1), 0xff, 0);
        put(s, (uint8_t)(base + 2), 0xff, 0);
        put(s, (uint8_t)(base + 3), 0xff, 0);
    }
    put(s, REG_DEVICE_MODE, 0x00, MCLW_REG_WRITABLE);
    put(s, REG_GEST_ID, 0x00, 0);
    put(s, REG_ID_G_THGROUP, 0x00, MCLW_REG_WRITABLE);
    put(s, REG_ID_G_CIPHER, CIPHER, 0);
    put(s, REG_ID_G_MODE, 0x00, MCLW_REG_WRITABLE);
    put(s, REG_ID_G_PMODE, 0x00, MCLW_REG_WRITABLE);
    put(s, REG_ID_G_FIRMID, 0x00, 0);
    put(s, REG_ID_G_STATE, 0x01, 0);
    put(s, REG_ID_G_VENDOR, 0x00, 0);
    s->reported_down = false;
    publish(s);
}

static int ft5336_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct ft5336_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->int_pin = bindings[BINDING_INT].present ? bindings[BINDING_INT].gpio : NULL;
    s->x = 0;
    s->y = 0;
    s->pressed = false;
    power_on(s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void ft5336_deinit(struct mclw_instance *inst) {
    struct ft5336_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    if (s->int_pin != NULL) {
        mclw_hal_gpio_release(s->int_pin);
    }
    s->i2c = NULL;
    s->int_pin = NULL;
}

static int ft5336_reset(struct mclw_instance *inst, bool factory) {
    struct ft5336_state *s = inst->state;

    (void)factory;
    power_on(s);
    return 0;
}

// Once a millisecond: a point put down reads as contact from the next
// update on (2.1.4's event flags).
static void ft5336_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct ft5336_state *s = inst->state;

    (void)now_us;
    if (s->pressed && !s->reported_down) {
        s->reported_down = true;
        publish(s);
    }
}

static int ft5336_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct ft5336_state *s = inst->state;

    switch (property) {
        case PROP_POSITION:
            out->v[0] = s->x;
            out->v[1] = s->y;
            return 0;
        case PROP_PRESSED:
            out->v[0] = s->pressed;
            return 0;
        default:
            return -EINVAL;
    }
}

static int ft5336_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct ft5336_state *s = inst->state;

    switch (property) {
        case PROP_POSITION:
            s->x = (int32_t)in->v[0];
            s->y = (int32_t)in->v[1];
            break;
        case PROP_PRESSED:
            s->pressed = in->v[0] != 0;
            s->reported_down = false;
            break;
        default:
            return -EINVAL;
    }
    publish(s);
    return 0;
}

static const struct mclw_model_ops ft5336_ops = {
    .init = ft5336_init,
    .deinit = ft5336_deinit,
    .reset = ft5336_reset,
    .get = ft5336_get,
    .set = ft5336_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = ft5336_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_ft5336 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "ft5336",
    .family = "ft5336",
    .config = NULL,
    .properties = ft5336_properties,
    .property_count = sizeof(ft5336_properties) / sizeof(ft5336_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = ft5336_bindings,
    .binding_count = sizeof(ft5336_bindings) / sizeof(ft5336_bindings[0]),
    .state_size = sizeof(struct ft5336_state),
    .ops = &ft5336_ops,
};
