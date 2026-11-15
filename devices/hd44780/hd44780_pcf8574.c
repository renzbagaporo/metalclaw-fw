// SPDX-License-Identifier: Apache-2.0 OR MIT
// An HD44780 character LCD behind a PCF8574 I2C backpack, as a sink. The
// backpack's port drives the LCD's pins: P0 RS, P1 R/W, P2 E, P3 the
// backlight transistor, P4-P7 DB4-DB7, with DB0-DB3 left open. That is the
// common "LCM1602 IIC" wiring, and the one pico-examples' lcd_1602_i2c.c
// assumes in its bit constants.
//
// PCF8574: TI SCPS068K (Sep 2024). HD44780U: ADE-207-272(Z) Rev 0.0.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/sink.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "hd44780.h"
#include "hd44780_core.h"

#define PORT_RS 0x01u
#define PORT_RW 0x02u
#define PORT_E 0x04u
#define PORT_BACKLIGHT 0x08u
#define PORT_DB 0xf0u
// DB0-DB3 are open on the backpack; the HD44780U's DB pins have pull-up
// MOS (p. 54, note 5), so in 8-bit mode they read 1.
#define OPEN_DB 0x0fu

enum {
    PROP_PORT,
    PROP_BACKLIGHT,
    PROP_DISPLAY_ON,
    PROP_CURSOR,
    PROP_BLINK,
    PROP_TWO_LINES,
    PROP_FONT_5X10,
    PROP_EIGHT_BIT,
    PROP_INCREMENT,
    PROP_SHIFT_ON_WRITE,
    PROP_ADDRESS,
    PROP_CGRAM,
    PROP_SHIFT,
    PROP_INSTRUCTIONS,
    PROP_WRITES,
};

enum {
    REGION_DDRAM,
    REGION_CGRAM,
};

enum {
    BINDING_I2C,
};

struct hd44780_pcf8574_state {
    struct hd44780 lcd;
    uint8_t port;
    struct mclw_sink sink;
    struct mclw_i2c_target *i2c;
};

#define RO(property_name, property_type, property_max) \
    { \
        .name = (property_name), \
        .type = (property_type), \
        .count = 1, \
        .access = MCLW_ACCESS_RO, \
        .unit = NULL, \
        .min = 0, \
        .max = (property_max), \
    }

static const struct mclw_property_desc hd44780_pcf8574_properties[] = {
    RO("port", MCLW_TYPE_U32, 255),
    RO("backlight", MCLW_TYPE_BOOL, 1),
    RO("display_on", MCLW_TYPE_BOOL, 1),
    RO("cursor", MCLW_TYPE_BOOL, 1),
    RO("blink", MCLW_TYPE_BOOL, 1),
    RO("two_lines", MCLW_TYPE_BOOL, 1),
    RO("font_5x10", MCLW_TYPE_BOOL, 1),
    RO("eight_bit", MCLW_TYPE_BOOL, 1),
    RO("increment", MCLW_TYPE_BOOL, 1),
    RO("shift_on_write", MCLW_TYPE_BOOL, 1),
    RO("address", MCLW_TYPE_U32, 127),
    RO("cgram", MCLW_TYPE_BOOL, 1),
    RO("shift", MCLW_TYPE_U32, 79),
    RO("instructions", MCLW_TYPE_U32, UINT32_MAX),
    RO("writes", MCLW_TYPE_U32, UINT32_MAX),
};

static const struct mclw_region_desc hd44780_pcf8574_regions[] = {
    { .name = "ddram", .size = HD44780_DDRAM_BYTES, .access = MCLW_ACCESS_RO },
    { .name = "cgram", .size = HD44780_CGRAM_BYTES, .access = MCLW_ACCESS_RO },
};

static const struct mclw_binding_desc hd44780_pcf8574_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// A new port value. The LCD latches a write at E's fall with the RS, R/W
// and data it had while E was high (write operation timing, p. 49; figure 25); with
// R/W high it was a read, which the model does not answer.
static void port_write(struct hd44780_pcf8574_state *state, uint8_t value) {
    uint8_t was = state->port;

    if ((was & PORT_E) != 0 && (value & PORT_E) == 0 && (was & PORT_RW) == 0) {
        hd44780_write(&state->lcd, (was & PORT_RS) != 0, (uint8_t)((was & PORT_DB) | OPEN_DB));
    }
    state->port = value;
    // A read returns the P port (SCPS068K 7.3.1); a quasi-bidirectional pin
    // reads what was written unless something pulls it low, and nothing does.
    state->sink.read_byte = value;
}

// SCPS068K 7.3.1: the first data byte after the address goes to the port;
// further bytes in the same transfer are ignored.
static void on_bytes(struct mclw_sink *sink, const uint16_t *entries, size_t count, bool ended, void *ctx) {
    struct hd44780_pcf8574_state *state = ctx;
    (void)sink;
    (void)ended;

    for (size_t i = 0; i < count; i++) {
        if ((entries[i] & MCLW_SINK_FIRST) != 0) {
            port_write(state, MCLW_SINK_BYTE(entries[i]));
        }
    }
    if (count > 0) {
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_I2C, MCLW_SINK_BYTE(entries[0]),
            (uint16_t)(count > UINT16_MAX ? UINT16_MAX : count), 0);
    }
}

// At power-on the I/Os are high (SCPS068K section 3).
static int power_on(struct hd44780_pcf8574_state *state) {
    hd44780_power_on(&state->lcd);
    mclw_sink_init(&state->sink, on_bytes, state);
    state->port = 0xff;
    state->sink.read_byte = state->port;
    return mclw_hal_i2c_target_arm_sink(state->i2c, &state->sink);
}

static int hd44780_pcf8574_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct hd44780_pcf8574_state *state = inst->state;

    state->i2c = bindings[BINDING_I2C].i2c;
    return power_on(state);
}

static void hd44780_pcf8574_deinit(struct mclw_instance *inst) {
    struct hd44780_pcf8574_state *state = inst->state;

    mclw_hal_i2c_target_disarm(state->i2c);
    state->i2c = NULL;
}

static int hd44780_pcf8574_reset(struct mclw_instance *inst, bool factory) {
    struct hd44780_pcf8574_state *state = inst->state;
    (void)factory;

    mclw_hal_i2c_target_disarm(state->i2c);
    return power_on(state);
}

static int hd44780_pcf8574_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct hd44780_pcf8574_state *state = inst->state;
    const struct hd44780 *h = &state->lcd;

    switch (property) {
        case PROP_PORT:
            out->v[0] = state->port;
            return 0;
        case PROP_BACKLIGHT:
            out->v[0] = (state->port & PORT_BACKLIGHT) != 0;
            return 0;
        case PROP_DISPLAY_ON:
            out->v[0] = h->display_on;
            return 0;
        case PROP_CURSOR:
            out->v[0] = h->cursor;
            return 0;
        case PROP_BLINK:
            out->v[0] = h->blink;
            return 0;
        case PROP_TWO_LINES:
            out->v[0] = h->two_lines;
            return 0;
        case PROP_FONT_5X10:
            out->v[0] = h->font_5x10;
            return 0;
        case PROP_EIGHT_BIT:
            out->v[0] = h->eight_bit;
            return 0;
        case PROP_INCREMENT:
            out->v[0] = h->increment;
            return 0;
        case PROP_SHIFT_ON_WRITE:
            out->v[0] = h->shift_on_write;
            return 0;
        case PROP_ADDRESS:
            out->v[0] = h->ac;
            return 0;
        case PROP_CGRAM:
            out->v[0] = h->cgram_selected;
            return 0;
        case PROP_SHIFT:
            out->v[0] = h->shift;
            return 0;
        case PROP_INSTRUCTIONS:
            out->v[0] = h->instructions;
            return 0;
        case PROP_WRITES:
            out->v[0] = h->writes;
            return 0;
        default:
            return -EINVAL;
    }
}

static int hd44780_pcf8574_read_region(struct mclw_instance *inst, size_t region, uint32_t offset,
    void *buf, size_t len) {
    const struct hd44780_pcf8574_state *state = inst->state;

    switch (region) {
        case REGION_DDRAM:
            memcpy(buf, &state->lcd.ddram[offset], len);
            return 0;
        case REGION_CGRAM:
            memcpy(buf, &state->lcd.cgram[offset], len);
            return 0;
        default:
            return -EINVAL;
    }
}

static const struct mclw_model_ops hd44780_pcf8574_ops = {
    .init = hd44780_pcf8574_init,
    .deinit = hd44780_pcf8574_deinit,
    .reset = hd44780_pcf8574_reset,
    .get = hd44780_pcf8574_get,
    .set = NULL,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = hd44780_pcf8574_read_region,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_hd44780_pcf8574 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "hd44780_pcf8574",
    .family = "hd44780",
    .config = NULL,
    .properties = hd44780_pcf8574_properties,
    .property_count = sizeof(hd44780_pcf8574_properties) / sizeof(hd44780_pcf8574_properties[0]),
    .regions = hd44780_pcf8574_regions,
    .region_count = 2,
    .faults = NULL,
    .fault_count = 0,
    .bindings = hd44780_pcf8574_bindings,
    .binding_count = sizeof(hd44780_pcf8574_bindings) / sizeof(hd44780_pcf8574_bindings[0]),
    .state_size = sizeof(struct hd44780_pcf8574_state),
    .ops = &hd44780_pcf8574_ops,
};
