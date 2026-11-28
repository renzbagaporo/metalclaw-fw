// SPDX-License-Identifier: Apache-2.0 OR MIT
// TI SN754410 quadruple half-H driver (SLRS007C), seen from its inputs:
// each one's edges timed by an EDGES_IN wire, the outputs taken from the
// function table (9.4, Table 1) and each pair's drive from the inputs'
// duties. An input left unwired reads high, as an open TTL input of the
// part approaches VCC1 (9.3.2).

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/edges.h"
#include "metalclaw/hal/wire.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "sn754410.h"

enum {
    PROP_LEVELS,
    PROP_DUTY_PPM,
    PROP_PERIOD_NS,
    PROP_OUTPUTS,
    PROP_DRIVE_PPM,
    PROP_MISSED,
    PROP_IDLE_MS,
};

// The inputs in binding order: 1A, 2A, 1,2EN, 3A, 4A, 3,4EN.
enum {
    IN_1A,
    IN_2A,
    IN_12EN,
    IN_3A,
    IN_4A,
    IN_34EN,
    INPUTS,
};

#define OUTPUTS 4u
#define BRIDGES 2u
#define PPM 1000000
#define IDLE_MS_DEFAULT 100u
// Output states as `outputs` reports them.
#define OUT_UNKNOWN (-1)
#define OUT_LOW 0
#define OUT_HIGH 1
#define OUT_OFF 2

struct sn754410_state {
    struct mclw_wire *in[INPUTS];
    struct mclw_edges edges[INPUTS];
    uint32_t idle_ms;
};

static const struct mclw_property_desc sn754410_properties[] = {
    {
        .name = "levels",
        .type = MCLW_TYPE_I32,
        .count = INPUTS,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = -1,
        .max = 1,
    },
    {
        .name = "duty_ppm",
        .type = MCLW_TYPE_U32,
        .count = INPUTS,
        .access = MCLW_ACCESS_RO,
        .unit = "ppm",
        .min = 0,
        .max = PPM,
    },
    {
        .name = "period_ns",
        .type = MCLW_TYPE_U32,
        .count = INPUTS,
        .access = MCLW_ACCESS_RO,
        .unit = "ns",
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "outputs",
        .type = MCLW_TYPE_I32,
        .count = OUTPUTS,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = OUT_UNKNOWN,
        .max = OUT_OFF,
    },
    {
        .name = "drive_ppm",
        .type = MCLW_TYPE_I32,
        .count = BRIDGES,
        .access = MCLW_ACCESS_RO,
        .unit = "ppm",
        .min = -PPM,
        .max = PPM,
    },
    {
        .name = "missed",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "idle_ms",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "ms",
        .min = 1,
        .max = 60000,
    },
};

#define INPUT(binding_name, binding_required) \
    { \
        .name = (binding_name), \
        .kind = MCLW_BINDING_WIRE, \
        .required = (binding_required), \
        .usb = NULL, \
        .spi = { 0 }, \
        .uart = { 0 }, \
        .wire = { .engine = MCLW_WIRE_EDGES_IN, .pins = 1 }, \
    }

static const struct mclw_binding_desc sn754410_bindings[] = {
    INPUT("a1", true),
    INPUT("a2", false),
    INPUT("en12", false),
    INPUT("a3", false),
    INPUT("a4", false),
    INPUT("en34", false),
};

static int level_of(const struct sn754410_state *state, size_t input) {
    return state->in[input] == NULL ? 1 : state->edges[input].level;
}

static uint32_t duty_of(const struct sn754410_state *state, size_t input, uint64_t now) {
    if (state->in[input] == NULL) {
        return PPM;
    }
    return mclw_edges_duty_ppm(&state->edges[input], now, state->idle_ms * 1000u);
}

// Table 1: EN high passes A through; EN low turns the output off.
static int output_of(const struct sn754410_state *state, size_t output) {
    size_t a = output < 2u ? output : output + 1u;
    int en = level_of(state, output < 2u ? IN_12EN : IN_34EN);
    int level = level_of(state, a);

    if (en == 0) {
        return OUT_OFF;
    }
    if (en < 0 || level < 0) {
        return OUT_UNKNOWN;
    }
    return level != 0 ? OUT_HIGH : OUT_LOW;
}

// The pair's outputs' mean difference as a share of VCC2, 1Y over 2Y (3Y
// over 4Y) positive: (d(1A) - d(2A)) x d(EN), which is exact while one of
// the three inputs switches and the others hold, as sign-magnitude drive
// (PWM on EN, or on one A with the other held) and locked anti-phase
// (complementary PWM on both A) have it.
static int32_t drive_of(const struct sn754410_state *state, size_t bridge, uint64_t now) {
    size_t base = bridge * 3u;
    int64_t a = duty_of(state, base + IN_1A, now);
    int64_t b = duty_of(state, base + IN_2A, now);
    int64_t en = duty_of(state, base + IN_12EN, now);

    return (int32_t)((a - b) * en / PPM);
}

static void sn754410_poll(struct mclw_instance *inst) {
    struct sn754410_state *state = inst->state;
    uint64_t now = mclw_now_us();

    for (size_t i = 0; i < INPUTS; i++) {
        if (state->in[i] != NULL) {
            mclw_edges_poll(&state->edges[i], state->in[i], now);
        }
    }
}

static int sn754410_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct sn754410_state *state = inst->state;

    for (size_t i = 0; i < INPUTS; i++) {
        state->in[i] = bindings[i].present ? bindings[i].wire : NULL;
        mclw_edges_init(&state->edges[i]);
    }
    state->idle_ms = IDLE_MS_DEFAULT;
    return 0;
}

static void sn754410_deinit(struct mclw_instance *inst) {
    struct sn754410_state *state = inst->state;

    for (size_t i = 0; i < INPUTS; i++) {
        state->in[i] = NULL;
    }
}

static int sn754410_reset(struct mclw_instance *inst, bool factory) {
    struct sn754410_state *state = inst->state;

    for (size_t i = 0; i < INPUTS; i++) {
        int8_t level = state->edges[i].level;
        uint32_t overruns = state->edges[i].overruns_seen;

        mclw_edges_init(&state->edges[i]);
        state->edges[i].level = level;
        state->edges[i].overruns_seen = overruns;
    }
    if (factory) {
        state->idle_ms = IDLE_MS_DEFAULT;
    }
    return 0;
}

static int sn754410_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct sn754410_state *state = inst->state;
    uint64_t now = mclw_now_us();

    switch (property) {
        case PROP_LEVELS:
            for (size_t i = 0; i < INPUTS; i++) {
                out->v[i] = level_of(state, i);
            }
            return 0;
        case PROP_DUTY_PPM:
            for (size_t i = 0; i < INPUTS; i++) {
                out->v[i] = duty_of(state, i, now);
            }
            return 0;
        case PROP_PERIOD_NS:
            for (size_t i = 0; i < INPUTS; i++) {
                bool timed = state->in[i] != NULL &&
                    !mclw_edges_still(&state->edges[i], now, state->idle_ms * 1000u);
                out->v[i] = timed ? state->edges[i].period_ns : 0u;
            }
            return 0;
        case PROP_OUTPUTS:
            for (size_t i = 0; i < OUTPUTS; i++) {
                out->v[i] = output_of(state, i);
            }
            return 0;
        case PROP_DRIVE_PPM:
            for (size_t i = 0; i < BRIDGES; i++) {
                out->v[i] = drive_of(state, i, now);
            }
            return 0;
        case PROP_MISSED: {
            uint32_t missed = 0;
            for (size_t i = 0; i < INPUTS; i++) {
                missed += state->edges[i].missed;
            }
            out->v[0] = missed;
            return 0;
        }
        case PROP_IDLE_MS:
            out->v[0] = state->idle_ms;
            return 0;
        default:
            return -EINVAL;
    }
}

static int sn754410_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct sn754410_state *state = inst->state;

    if (property != PROP_IDLE_MS) {
        return -EINVAL;
    }
    state->idle_ms = (uint32_t)in->v[0];
    return 0;
}

static const struct mclw_model_ops sn754410_ops = {
    .init = sn754410_init,
    .deinit = sn754410_deinit,
    .reset = sn754410_reset,
    .get = sn754410_get,
    .set = sn754410_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = sn754410_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_sn754410 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "sn754410",
    .family = "sn754410",
    .config = NULL,
    .properties = sn754410_properties,
    .property_count = sizeof(sn754410_properties) / sizeof(sn754410_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = sn754410_bindings,
    .binding_count = sizeof(sn754410_bindings) / sizeof(sn754410_bindings[0]),
    .state_size = sizeof(struct sn754410_state),
    .ops = &sn754410_ops,
};
