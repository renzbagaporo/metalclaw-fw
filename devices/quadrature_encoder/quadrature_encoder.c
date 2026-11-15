// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/wire.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "quadrature_encoder.h"

enum {
    PROP_RATE,
    PROP_TRAVEL,
    PROP_POSITION,
    PROP_STEPS,
};

enum {
    BINDING_AB,
};

#define STATES_PER_WORD 16u
#define WRITE_BATCH 8u

struct quadrature_encoder_state {
    struct mclw_wire *ab;
    int32_t rate;
    uint32_t travel;
    // Where the shaft is, as queued for the pins, and how far it has gone
    // on its current stroke; the direction it turns.
    int32_t position;
    uint32_t stroke;
    int32_t direction;
    uint32_t steps;
    // The rate the engine is paced for; a new one waits for the queue to
    // run out.
    int32_t paced;
};

static const struct mclw_property_desc quadrature_encoder_properties[] = {
    {
        .name = "rate",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "steps/s",
        .min = -QUADRATURE_ENCODER_RATE_MAX,
        .max = QUADRATURE_ENCODER_RATE_MAX,
    },
    {
        .name = "travel",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "steps",
        .min = 0,
        .max = INT32_MAX,
    },
    {
        .name = "position",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = "steps",
        .min = INT32_MIN,
        .max = INT32_MAX,
    },
    {
        .name = "steps",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
};

static const struct mclw_binding_desc quadrature_encoder_bindings[] = {
    {
        .name = "ab",
        .kind = MCLW_BINDING_WIRE,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
        .wire = { .engine = MCLW_WIRE_STEPS_OUT, .pins = 2 },
    },
};

// Channel A in bit 0, B in bit 1. Forward, B leads A by a quarter cycle:
// 00, 10, 11, 01. Position 0 is both low.
uint32_t quadrature_encoder_phase(int32_t position) {
    static const uint8_t gray[4] = { 0u, 2u, 3u, 1u };

    return gray[(uint32_t)position & 3u];
}

// One period of motion: a step in the current direction, turning back at
// either end of the travel when there is one.
static void advance(struct quadrature_encoder_state *state) {
    if (state->travel != 0u && state->stroke >= state->travel) {
        state->direction = -state->direction;
        state->stroke = 0;
    }
    state->position += state->direction;
    state->stroke++;
    state->steps++;
}

// The queue holds up to the engine's FIFO of words, each 16 periods; a new
// rate waits for it to run out, and the motion goes on from where it was.
static void quadrature_encoder_poll(struct mclw_instance *inst) {
    struct quadrature_encoder_state *state = inst->state;

    if (state->rate != state->paced) {
        uint32_t speed = (uint32_t)(state->rate < 0 ? -state->rate : state->rate);
        if (speed != 0u && mclw_hal_wire_set_period(state->ab, 1000000000u / speed) != 0) {
            return;
        }
        if (state->paced == 0 || state->rate == 0 || (state->rate < 0) != (state->paced < 0)) {
            state->direction = state->rate < 0 ? -1 : 1;
            state->stroke = 0;
        }
        state->paced = state->rate;
    }
    for (size_t i = 0; state->paced != 0 && i < WRITE_BATCH; i++) {
        struct quadrature_encoder_state before = *state;
        uint32_t word = 0;
        for (uint32_t s = 0; s < STATES_PER_WORD; s++) {
            advance(state);
            word |= quadrature_encoder_phase(state->position) << (2u * s);
        }
        if (mclw_hal_wire_write(state->ab, &word, 1) == 0u) {
            *state = before;
            return;
        }
    }
}

static int quadrature_encoder_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct quadrature_encoder_state *state = inst->state;

    state->ab = bindings[BINDING_AB].wire;
    state->rate = 0;
    state->paced = 0;
    state->travel = 0;
    state->position = 0;
    state->stroke = 0;
    state->direction = 1;
    state->steps = 0;
    return 0;
}

static void quadrature_encoder_deinit(struct mclw_instance *inst) {
    struct quadrature_encoder_state *state = inst->state;

    state->ab = NULL;
}

static int quadrature_encoder_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct quadrature_encoder_state *state = inst->state;

    switch (property) {
        case PROP_RATE:
            out->v[0] = state->rate;
            return 0;
        case PROP_TRAVEL:
            out->v[0] = state->travel;
            return 0;
        case PROP_POSITION:
            out->v[0] = state->position;
            return 0;
        case PROP_STEPS:
            out->v[0] = state->steps;
            return 0;
        default:
            return -EINVAL;
    }
}

static int quadrature_encoder_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct quadrature_encoder_state *state = inst->state;

    switch (property) {
        case PROP_RATE:
            state->rate = (int32_t)in->v[0];
            return 0;
        case PROP_TRAVEL:
            state->travel = (uint32_t)in->v[0];
            state->stroke = 0;
            return 0;
        default:
            return -EINVAL;
    }
}

static const struct mclw_model_ops quadrature_encoder_ops = {
    .init = quadrature_encoder_init,
    .deinit = quadrature_encoder_deinit,
    .reset = NULL,
    .get = quadrature_encoder_get,
    .set = quadrature_encoder_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = quadrature_encoder_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_quadrature_encoder = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "quadrature_encoder",
    .family = "quadrature_encoder",
    .config = NULL,
    .properties = quadrature_encoder_properties,
    .property_count = sizeof(quadrature_encoder_properties) / sizeof(quadrature_encoder_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = quadrature_encoder_bindings,
    .binding_count = sizeof(quadrature_encoder_bindings) / sizeof(quadrature_encoder_bindings[0]),
    .state_size = sizeof(struct quadrature_encoder_state),
    .ops = &quadrature_encoder_ops,
};
