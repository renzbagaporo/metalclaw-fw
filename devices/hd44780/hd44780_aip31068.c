// SPDX-License-Identifier: Apache-2.0 OR MIT
// A Wuxi I-CORE AIP31068L LCD controller on I2C, as a sink: the HD44780U's
// instruction set behind a two-wire serial port, as in a JHD1313 module
// (the LCD half of Seeed's Grove LCD RGB Backlight). The controller answers
// at 3Eh. Datasheet AIP31068L-AX-BJ-153EN; HD44780U ADE-207-272(Z) Rev 0.0
// for the instruction set the core decodes.

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

enum {
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

struct hd44780_aip31068_state {
    struct hd44780 lcd;
    struct mclw_sink sink;
    struct mclw_i2c_target *i2c;
    // In a transfer: whether the next byte is a control byte, and the RS
    // the last one gave.
    bool control_next;
    bool rs;
    // Whether the last control byte had Co set: one data byte, then another
    // control byte. A transfer may arrive in several deliveries.
    bool co;
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

static const struct mclw_property_desc hd44780_aip31068_properties[] = {
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

static const struct mclw_region_desc hd44780_aip31068_regions[] = {
    { .name = "ddram", .size = HD44780_DDRAM_BYTES, .access = MCLW_ACCESS_RO },
    { .name = "cgram", .size = HD44780_CGRAM_BYTES, .access = MCLW_ACCESS_RO },
};

static const struct mclw_binding_desc hd44780_aip31068_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// The 2-line serial interface (the datasheet's protocol figure): after the
// address, a control byte, Co in bit 7 and RS in bit 6. With Co set one
// byte follows and then another control byte; with Co clear every byte to
// the STOP is a data byte at that RS: an instruction with RS low, a DDRAM
// or CGRAM write with it high. The interface is 8 bits wide.
static void on_bytes(struct mclw_sink *sink, const uint16_t *entries, size_t count, bool ended, void *ctx) {
    struct hd44780_aip31068_state *state = ctx;

    (void)sink;
    (void)ended;
    for (size_t i = 0; i < count; i++) {
        uint8_t byte = MCLW_SINK_BYTE(entries[i]);

        if ((entries[i] & MCLW_SINK_FIRST) != 0) {
            state->control_next = true;
        }
        if (state->control_next) {
            state->rs = (byte & 0x40u) != 0;
            state->co = (byte & 0x80u) != 0;
            state->control_next = false;
            continue;
        }
        hd44780_write(&state->lcd, state->rs, byte);
        state->control_next = state->co;
    }
    if (count > 0) {
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_I2C, MCLW_SINK_BYTE(entries[0]),
            (uint16_t)(count > UINT16_MAX ? UINT16_MAX : count), 0);
    }
}

// The internal reset (the HD44780U's, which the AIP31068L shares), then
// the 8-bit interface its serial port implies.
static int power_on(struct hd44780_aip31068_state *state) {
    hd44780_power_on(&state->lcd);
    state->lcd.eight_bit = true;
    state->control_next = true;
    state->rs = false;
    state->co = false;
    mclw_sink_init(&state->sink, on_bytes, state);
    state->sink.read_byte = 0;
    return mclw_hal_i2c_target_arm_sink(state->i2c, &state->sink);
}

static int hd44780_aip31068_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct hd44780_aip31068_state *state = inst->state;

    state->i2c = bindings[BINDING_I2C].i2c;
    return power_on(state);
}

static void hd44780_aip31068_deinit(struct mclw_instance *inst) {
    struct hd44780_aip31068_state *state = inst->state;

    mclw_hal_i2c_target_disarm(state->i2c);
    state->i2c = NULL;
}

static int hd44780_aip31068_reset(struct mclw_instance *inst, bool factory) {
    struct hd44780_aip31068_state *state = inst->state;
    (void)factory;

    mclw_hal_i2c_target_disarm(state->i2c);
    return power_on(state);
}

static int hd44780_aip31068_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct hd44780_aip31068_state *state = inst->state;
    const struct hd44780 *h = &state->lcd;

    switch (property) {
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

static int hd44780_aip31068_read_region(struct mclw_instance *inst, size_t region, uint32_t offset,
    void *buf, size_t len) {
    const struct hd44780_aip31068_state *state = inst->state;

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

static const struct mclw_model_ops hd44780_aip31068_ops = {
    .init = hd44780_aip31068_init,
    .deinit = hd44780_aip31068_deinit,
    .reset = hd44780_aip31068_reset,
    .get = hd44780_aip31068_get,
    .set = NULL,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = hd44780_aip31068_read_region,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_aip31068 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "aip31068",
    .family = "hd44780",
    .config = NULL,
    .properties = hd44780_aip31068_properties,
    .property_count = sizeof(hd44780_aip31068_properties) / sizeof(hd44780_aip31068_properties[0]),
    .regions = hd44780_aip31068_regions,
    .region_count = 2,
    .faults = NULL,
    .fault_count = 0,
    .bindings = hd44780_aip31068_bindings,
    .binding_count = sizeof(hd44780_aip31068_bindings) / sizeof(hd44780_aip31068_bindings[0]),
    .state_size = sizeof(struct hd44780_aip31068_state),
    .ops = &hd44780_aip31068_ops,
};
