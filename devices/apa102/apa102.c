// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/wire.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "apa102.h"

enum {
    PROP_COUNT,
    PROP_FRAMES,
    PROP_LEDS_SET,
    PROP_END_FRAMES,
};

enum {
    BINDING_SPI,
};

enum {
    REGION_LEDS,
    REGION_FRAME,
};

#define READ_BATCH 32u
#define START_FRAME_BITS 32u

struct apa102_state {
    struct mclw_wire *spi;
    uint32_t count;
    uint32_t frames;
    uint32_t leds_set;
    uint32_t end_frames;
    // Zeros in a row, the frame being shifted in and its bits, and which
    // LED it is for; -1 until a start frame has been seen.
    uint32_t zeros;
    uint32_t word;
    uint8_t bits;
    int32_t led;
    uint8_t leds[APA102_LEDS_MAX * 4u];
    // The strip as its last end frame left it: a whole update, where
    // `leds` is mid-way through the next one while it shifts in.
    uint8_t frame[APA102_LEDS_MAX * 4u];
};

static const struct mclw_property_desc apa102_properties[] = {
    {
        .name = "count",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 1,
        .max = APA102_LEDS_MAX,
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
        .name = "leds_set",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "end_frames",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
};

static const struct mclw_region_desc apa102_regions[] = {
    { .name = "leds", .size = APA102_LEDS_MAX * 4u, .access = MCLW_ACCESS_RO },
    { .name = "frame", .size = APA102_LEDS_MAX * 4u, .access = MCLW_ACCESS_RO },
};

// APA102C datasheet p. 4: SDI is taken at each rising edge of CKI, MSB
// first. It gives no clock limit; the engine samples up to tens of MHz.
static const struct mclw_binding_desc apa102_bindings[] = {
    {
        .name = "spi",
        .kind = MCLW_BINDING_WIRE,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
        .wire = { .engine = MCLW_WIRE_CLOCKED_IN, .pins = 2 },
    },
};

// Datasheet p. 3, "cascading data structure": a start frame of 32 zeros,
// then a 32-bit frame per LED, 111 and the 5-bit global brightness, then
// blue, green and red, MSB first; an end frame of 32 ones. An LED frame
// starts with ones, so at most 29 zeros run inside the data: 32 are always
// a start frame, and the first 1 after one starts the first LED's frame.
static void take_bit(struct apa102_state *state, bool bit) {
    if (!bit) {
        if (++state->zeros == START_FRAME_BITS) {
            state->led = 0;
            state->bits = 0;
            state->frames++;
            return;
        }
    } else {
        state->zeros = 0;
    }
    if (state->led < 0 || (state->bits == 0u && !bit)) {
        return;
    }
    state->word = (state->word << 1) | (bit ? 1u : 0u);
    if (++state->bits < 32u) {
        return;
    }
    state->bits = 0;
    if ((uint32_t)state->led < state->count) {
        uint8_t *led = &state->leds[(uint32_t)state->led * 4u];
        led[0] = (uint8_t)(state->word >> 24);
        led[1] = (uint8_t)(state->word >> 16);
        led[2] = (uint8_t)(state->word >> 8);
        led[3] = (uint8_t)state->word;
        state->leds_set++;
    } else if (state->word == 0xffffffffu) {
        state->end_frames++;
        memcpy(state->frame, state->leds, sizeof(state->frame));
    }
    state->led++;
}

static void apa102_poll(struct mclw_instance *inst) {
    struct apa102_state *state = inst->state;
    uint32_t words[READ_BATCH];
    size_t n;

    while ((n = mclw_hal_wire_read(state->spi, words, READ_BATCH)) > 0u) {
        for (size_t i = 0; i < n; i++) {
            for (int b = 31; b >= 0; b--) {
                take_bit(state, ((words[i] >> b) & 1u) != 0u);
            }
        }
    }
}

static void power_up(struct apa102_state *state) {
    state->frames = 0;
    state->leds_set = 0;
    state->end_frames = 0;
    state->zeros = 0;
    state->word = 0;
    state->bits = 0;
    state->led = -1;
    memset(state->leds, 0, sizeof(state->leds));
    memset(state->frame, 0, sizeof(state->frame));
}

static int apa102_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct apa102_state *state = inst->state;

    state->spi = bindings[BINDING_SPI].wire;
    state->count = 1;
    power_up(state);
    return 0;
}

static void apa102_deinit(struct mclw_instance *inst) {
    struct apa102_state *state = inst->state;

    state->spi = NULL;
}

static int apa102_reset(struct mclw_instance *inst, bool factory) {
    (void)factory;
    power_up(inst->state);
    return 0;
}

static int apa102_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct apa102_state *state = inst->state;

    switch (property) {
        case PROP_COUNT:
            out->v[0] = state->count;
            return 0;
        case PROP_FRAMES:
            out->v[0] = state->frames;
            return 0;
        case PROP_LEDS_SET:
            out->v[0] = state->leds_set;
            return 0;
        case PROP_END_FRAMES:
            out->v[0] = state->end_frames;
            return 0;
        default:
            return -EINVAL;
    }
}

static int apa102_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct apa102_state *state = inst->state;

    if (property != PROP_COUNT) {
        return -EINVAL;
    }
    state->count = (uint32_t)in->v[0];
    return 0;
}

static int apa102_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    const struct apa102_state *state = inst->state;

    const uint8_t *data = region == REGION_LEDS ? state->leds : region == REGION_FRAME ? state->frame : NULL;

    if (data == NULL || offset > sizeof(state->leds) || len > sizeof(state->leds) - offset) {
        return -EINVAL;
    }
    memcpy(buf, &data[offset], len);
    return 0;
}

static const struct mclw_model_ops apa102_ops = {
    .init = apa102_init,
    .deinit = apa102_deinit,
    .reset = apa102_reset,
    .get = apa102_get,
    .set = apa102_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = apa102_read_region,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = apa102_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_apa102 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "apa102",
    .family = "apa102",
    .config = NULL,
    .properties = apa102_properties,
    .property_count = sizeof(apa102_properties) / sizeof(apa102_properties[0]),
    .regions = apa102_regions,
    .region_count = sizeof(apa102_regions) / sizeof(apa102_regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = apa102_bindings,
    .binding_count = sizeof(apa102_bindings) / sizeof(apa102_bindings[0]),
    .state_size = sizeof(struct apa102_state),
    .ops = &apa102_ops,
};
