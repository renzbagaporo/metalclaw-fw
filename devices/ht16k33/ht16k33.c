// SPDX-License-Identifier: Apache-2.0 OR MIT
// The Holtek HT16K33 RAM-mapped LED driver on I2C, as a sink: each write
// transaction starts with a command byte, and only the display data address
// pointer takes data after it. Holtek HT16K33 Rev 1.10 (May 16, 2011);
// page numbers below are from it.

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

#include "ht16k33.h"

enum {
    PROP_OSCILLATOR,
    PROP_DISPLAY_ON,
    PROP_BLINK,
    PROP_DIMMING,
    PROP_ROW_INT,
    PROP_ADDRESS,
};

enum {
    BINDING_I2C,
};

struct ht16k33_state {
    uint8_t display[HT16K33_DISPLAY_BYTES];
    uint8_t pointer;
    // The transaction began with the display data address pointer, so the
    // bytes after it are display data.
    bool data;
    bool oscillator;
    bool display_on;
    uint8_t blink;
    uint8_t dimming;
    uint8_t row_int;
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

static const struct mclw_property_desc ht16k33_properties[] = {
    RO("oscillator", MCLW_TYPE_BOOL, 1),
    RO("display_on", MCLW_TYPE_BOOL, 1),
    RO("blink", MCLW_TYPE_U32, 3),
    RO("dimming", MCLW_TYPE_U32, 15),
    RO("row_int", MCLW_TYPE_U32, 3),
    RO("address", MCLW_TYPE_U32, 15),
};

static const struct mclw_region_desc ht16k33_regions[] = {
    { .name = "display", .size = HT16K33_DISPLAY_BYTES, .access = MCLW_ACCESS_RO },
};

static const struct mclw_binding_desc ht16k33_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// Power-on reset (p. 8): oscillator off, display off, dimming 16/16 (EFh,
// p. 31), ROW/INT as ROW outputs, blinking off. The RAM's power-on content
// is not given; the model starts it clear.
static void power_on(struct ht16k33_state *state) {
    memset(state->display, 0, sizeof(state->display));
    state->pointer = 0;
    state->data = false;
    state->oscillator = false;
    state->display_on = false;
    state->blink = 0;
    state->dimming = 15;
    state->row_int = 0;
}

// The command summary (pp. 30-31). In standby only the system setup
// command is taken (p. 8); undefined commands change nothing (p. 30 note).
static void command(struct ht16k33_state *state, uint8_t byte) {
    uint8_t kind = byte & 0xf0u;

    state->data = false;
    if (!state->oscillator && kind != 0x20u) {
        return;
    }
    switch (kind) {
        case 0x00:
            state->pointer = byte & 0x0fu;
            state->data = true;
            break;
        case 0x20:
            state->oscillator = (byte & 0x01u) != 0;
            break;
        case 0x80:
            state->display_on = (byte & 0x01u) != 0;
            state->blink = (byte >> 1) & 0x03u;
            break;
        case 0xa0:
            state->row_int = byte & 0x03u;
            break;
        case 0xe0:
            state->dimming = byte & 0x0fu;
            break;
        default:
            break;
    }
}

// Page write (p. 28): the pointer moves on after each data byte and wraps
// from 0Fh to 00h.
static void on_bytes(struct mclw_sink *sink, const uint16_t *entries, size_t count, bool ended, void *ctx) {
    struct ht16k33_state *state = ctx;
    (void)sink;
    (void)ended;

    for (size_t i = 0; i < count; i++) {
        uint8_t byte = MCLW_SINK_BYTE(entries[i]);

        if ((entries[i] & MCLW_SINK_FIRST) != 0) {
            command(state, byte);
        } else if (state->data && state->oscillator) {
            state->display[state->pointer] = byte;
            state->pointer = (uint8_t)((state->pointer + 1u) & 0x0fu);
        }
    }
    if (count > 0) {
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_I2C, MCLW_SINK_BYTE(entries[0]),
            (uint16_t)(count > UINT16_MAX ? UINT16_MAX : count), 0);
    }
}

static int arm(struct ht16k33_state *state) {
    mclw_sink_init(&state->sink, on_bytes, state);
    // No key is ever pressed: the key data RAM and the INT flag read 0.
    state->sink.read_byte = 0x00;
    return mclw_hal_i2c_target_arm_sink(state->i2c, &state->sink);
}

static int ht16k33_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct ht16k33_state *state = inst->state;

    state->i2c = bindings[BINDING_I2C].i2c;
    power_on(state);
    return arm(state);
}

static void ht16k33_deinit(struct mclw_instance *inst) {
    struct ht16k33_state *state = inst->state;

    mclw_hal_i2c_target_disarm(state->i2c);
    state->i2c = NULL;
}

static int ht16k33_reset(struct mclw_instance *inst, bool factory) {
    struct ht16k33_state *state = inst->state;
    (void)factory;

    mclw_hal_i2c_target_disarm(state->i2c);
    power_on(state);
    return arm(state);
}

static int ht16k33_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct ht16k33_state *state = inst->state;

    switch (property) {
        case PROP_OSCILLATOR:
            out->v[0] = state->oscillator;
            return 0;
        case PROP_DISPLAY_ON:
            out->v[0] = state->display_on;
            return 0;
        case PROP_BLINK:
            out->v[0] = state->blink;
            return 0;
        case PROP_DIMMING:
            out->v[0] = state->dimming;
            return 0;
        case PROP_ROW_INT:
            out->v[0] = state->row_int;
            return 0;
        case PROP_ADDRESS:
            out->v[0] = state->pointer;
            return 0;
        default:
            return -EINVAL;
    }
}

static int ht16k33_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf,
    size_t len) {
    const struct ht16k33_state *state = inst->state;

    if (region != 0) {
        return -EINVAL;
    }
    memcpy(buf, &state->display[offset], len);
    return 0;
}

static const struct mclw_model_ops ht16k33_ops = {
    .init = ht16k33_init,
    .deinit = ht16k33_deinit,
    .reset = ht16k33_reset,
    .get = ht16k33_get,
    .set = NULL,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = ht16k33_read_region,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_ht16k33 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "ht16k33",
    .family = "ht16k33",
    .config = NULL,
    .properties = ht16k33_properties,
    .property_count = sizeof(ht16k33_properties) / sizeof(ht16k33_properties[0]),
    .regions = ht16k33_regions,
    .region_count = 1,
    .faults = NULL,
    .fault_count = 0,
    .bindings = ht16k33_bindings,
    .binding_count = sizeof(ht16k33_bindings) / sizeof(ht16k33_bindings[0]),
    .state_size = sizeof(struct ht16k33_state),
    .ops = &ht16k33_ops,
};
