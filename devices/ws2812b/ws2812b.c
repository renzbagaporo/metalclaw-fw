// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/wire.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "ws2812b.h"

enum {
    PROP_COUNT,
    PROP_CHANNELS,
    PROP_FRAMES,
    PROP_BAD_FRAMES,
    PROP_LENGTH,
};

enum {
    BINDING_DIN,
};

enum {
    REGION_LEDS,
};

#define READ_BATCH 64u

struct ws2812b_state {
    struct mclw_wire *din;
    uint32_t count;
    uint32_t channels;
    uint32_t frames;
    uint32_t bad_frames;
    uint32_t length;
    // The frame coming in, and whether a pulse in it was out of spec.
    uint32_t received;
    bool spoiled;
    uint8_t incoming[WS2812B_BYTES_MAX];
    uint8_t shown[WS2812B_BYTES_MAX];
};

static const struct mclw_property_desc ws2812b_properties[] = {
    {
        .name = "count",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 1,
        .max = WS2812B_LEDS_MAX,
    },
    {
        .name = "channels",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 3,
        .max = 4,
    },
    {
        .name = "frames",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "bad_frames",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "length",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = "bytes",
        .min = 0,
        .max = UINT32_MAX,
    },
};

static const struct mclw_region_desc ws2812b_regions[] = {
    { .name = "leds", .size = WS2812B_BYTES_MAX, .access = MCLW_ACCESS_RO },
};

// WS2812B datasheet, "Data transfer time": T0H 0.4 us and T1H 0.8 us, each
// +-150 ns; RES above 50 us.
static const struct mclw_binding_desc ws2812b_bindings[] = {
    {
        .name = "din",
        .kind = MCLW_BINDING_WIRE,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
        .wire = {
            .engine = MCLW_WIRE_PULSE_BITS_IN,
            .pins = 1,
            .pulse_bits = {
                .zero_min_ns = 250,
                .zero_max_ns = 550,
                .one_min_ns = 650,
                .one_max_ns = 950,
                .reset_ns = 50000,
            },
        },
    },
};

// A reset latches what each LED took: the first `channels` bytes of the
// frame went to the first LED, the next to the second, and so on; bytes
// beyond the strip went out of its last DOUT. A spoiled frame latches
// nothing.
static void ws2812b_take(struct ws2812b_state *state, const uint32_t *entries, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t e = entries[i];
        if (e == MCLW_WIRE_BAD) {
            state->spoiled = true;
            continue;
        }
        if (e != MCLW_WIRE_END) {
            if (state->received < WS2812B_BYTES_MAX) {
                state->incoming[state->received] = (uint8_t)e;
            }
            state->received++;
            continue;
        }
        if (state->spoiled) {
            state->bad_frames++;
        } else if (state->received > 0u) {
            uint32_t strip = state->count * state->channels;
            uint32_t taken = state->received < strip ? state->received / state->channels * state->channels : strip;
            memcpy(state->shown, state->incoming, taken);
            state->frames++;
            state->length = state->received;
        }
        state->received = 0;
        state->spoiled = false;
    }
}

static void ws2812b_poll(struct mclw_instance *inst) {
    struct ws2812b_state *state = inst->state;
    uint32_t entries[READ_BATCH];
    size_t n;

    while ((n = mclw_hal_wire_read(state->din, entries, READ_BATCH)) > 0u) {
        ws2812b_take(state, entries, n);
    }
}

static void power_up(struct ws2812b_state *state) {
    state->frames = 0;
    state->bad_frames = 0;
    state->length = 0;
    state->received = 0;
    state->spoiled = false;
    memset(state->shown, 0, sizeof(state->shown));
}

static int ws2812b_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct ws2812b_state *state = inst->state;

    state->din = bindings[BINDING_DIN].wire;
    state->count = 1;
    state->channels = 3;
    power_up(state);
    return 0;
}

static void ws2812b_deinit(struct mclw_instance *inst) {
    struct ws2812b_state *state = inst->state;

    state->din = NULL;
}

static int ws2812b_reset(struct mclw_instance *inst, bool factory) {
    (void)factory;
    power_up(inst->state);
    return 0;
}

static int ws2812b_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct ws2812b_state *state = inst->state;

    switch (property) {
        case PROP_COUNT:
            out->v[0] = state->count;
            return 0;
        case PROP_CHANNELS:
            out->v[0] = state->channels;
            return 0;
        case PROP_FRAMES:
            out->v[0] = state->frames;
            return 0;
        case PROP_BAD_FRAMES:
            out->v[0] = state->bad_frames;
            return 0;
        case PROP_LENGTH:
            out->v[0] = state->length;
            return 0;
        default:
            return -EINVAL;
    }
}

static int ws2812b_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct ws2812b_state *state = inst->state;

    switch (property) {
        case PROP_COUNT:
            state->count = (uint32_t)in->v[0];
            return 0;
        case PROP_CHANNELS:
            state->channels = (uint32_t)in->v[0];
            return 0;
        default:
            return -EINVAL;
    }
}

static int ws2812b_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    const struct ws2812b_state *state = inst->state;

    if (region != REGION_LEDS || offset > WS2812B_BYTES_MAX || len > WS2812B_BYTES_MAX - offset) {
        return -EINVAL;
    }
    memcpy(buf, &state->shown[offset], len);
    return 0;
}

static const struct mclw_model_ops ws2812b_ops = {
    .init = ws2812b_init,
    .deinit = ws2812b_deinit,
    .reset = ws2812b_reset,
    .get = ws2812b_get,
    .set = ws2812b_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = ws2812b_read_region,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = ws2812b_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_ws2812b = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "ws2812b",
    .family = "ws2812b",
    .config = NULL,
    .properties = ws2812b_properties,
    .property_count = sizeof(ws2812b_properties) / sizeof(ws2812b_properties[0]),
    .regions = ws2812b_regions,
    .region_count = sizeof(ws2812b_regions) / sizeof(ws2812b_regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = ws2812b_bindings,
    .binding_count = sizeof(ws2812b_bindings) / sizeof(ws2812b_bindings[0]),
    .state_size = sizeof(struct ws2812b_state),
    .ops = &ws2812b_ops,
};
