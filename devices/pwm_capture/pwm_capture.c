// SPDX-License-Identifier: Apache-2.0 OR MIT

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

#include "pwm_capture.h"

enum {
    PROP_PERIOD_NS,
    PROP_PULSE_NS,
    PROP_DUTY_PPM,
    PROP_STILL,
    PROP_LEVEL,
    PROP_PERIODS,
    PROP_MISSED,
    PROP_IDLE_MS,
};

enum {
    BINDING_PIN,
};

#define IDLE_MS_DEFAULT 100u

struct pwm_capture_state {
    struct mclw_wire *pin;
    struct mclw_edges edges;
    uint32_t idle_ms;
};

#define READ_ONLY(property_name, property_type, property_unit, property_min, property_max) \
    { \
        .name = (property_name), \
        .type = (property_type), \
        .count = 1, \
        .access = MCLW_ACCESS_RO, \
        .unit = (property_unit), \
        .min = (property_min), \
        .max = (property_max), \
    }

static const struct mclw_property_desc pwm_capture_properties[] = {
    READ_ONLY("period_ns", MCLW_TYPE_U32, "ns", 0, UINT32_MAX),
    READ_ONLY("pulse_ns", MCLW_TYPE_U32, "ns", 0, UINT32_MAX),
    READ_ONLY("duty_ppm", MCLW_TYPE_U32, "ppm", 0, 1000000),
    READ_ONLY("still", MCLW_TYPE_BOOL, NULL, 0, 1),
    READ_ONLY("level", MCLW_TYPE_I32, NULL, -1, 1),
    READ_ONLY("periods", MCLW_TYPE_U32, NULL, 0, UINT32_MAX),
    READ_ONLY("missed", MCLW_TYPE_U32, NULL, 0, UINT32_MAX),
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

static const struct mclw_binding_desc pwm_capture_bindings[] = {
    {
        .name = "pin",
        .kind = MCLW_BINDING_WIRE,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
        .wire = { .engine = MCLW_WIRE_EDGES_IN, .pins = 1 },
    },
};

static void pwm_capture_poll(struct mclw_instance *inst) {
    struct pwm_capture_state *state = inst->state;
    uint32_t periods = state->edges.periods;

    mclw_edges_poll(&state->edges, state->pin, mclw_now_us());
    if (state->edges.periods != periods) {
        mclw_trace(inst, MCLW_TRACE_STATE, state->edges.period_ns, 0, 0);
    }
}

static int pwm_capture_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct pwm_capture_state *state = inst->state;

    state->pin = bindings[BINDING_PIN].wire;
    state->idle_ms = IDLE_MS_DEFAULT;
    mclw_edges_init(&state->edges);
    return 0;
}

static void pwm_capture_deinit(struct mclw_instance *inst) {
    struct pwm_capture_state *state = inst->state;

    state->pin = NULL;
}

static int pwm_capture_reset(struct mclw_instance *inst, bool factory) {
    struct pwm_capture_state *state = inst->state;
    int8_t level = state->edges.level;
    uint32_t overruns = state->edges.overruns_seen;

    mclw_edges_init(&state->edges);
    state->edges.level = level;
    state->edges.overruns_seen = overruns;
    if (factory) {
        state->idle_ms = IDLE_MS_DEFAULT;
    }
    return 0;
}

static int pwm_capture_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct pwm_capture_state *state = inst->state;
    const struct mclw_edges *e = &state->edges;
    uint64_t now = mclw_now_us();
    uint32_t idle_us = state->idle_ms * 1000u;
    bool still = mclw_edges_still(e, now, idle_us);

    switch (property) {
        case PROP_PERIOD_NS:
            out->v[0] = e->period_ns;
            return 0;
        case PROP_PULSE_NS:
            out->v[0] = !still ? e->pulse_ns : e->level == 1 ? e->period_ns : 0u;
            return 0;
        case PROP_DUTY_PPM:
            out->v[0] = mclw_edges_duty_ppm(e, now, idle_us);
            return 0;
        case PROP_STILL:
            out->v[0] = still;
            return 0;
        case PROP_LEVEL:
            out->v[0] = e->level;
            return 0;
        case PROP_PERIODS:
            out->v[0] = e->periods;
            return 0;
        case PROP_MISSED:
            out->v[0] = e->missed;
            return 0;
        case PROP_IDLE_MS:
            out->v[0] = state->idle_ms;
            return 0;
        default:
            return -EINVAL;
    }
}

static int pwm_capture_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct pwm_capture_state *state = inst->state;

    if (property != PROP_IDLE_MS) {
        return -EINVAL;
    }
    state->idle_ms = (uint32_t)in->v[0];
    return 0;
}

static const struct mclw_model_ops pwm_capture_ops = {
    .init = pwm_capture_init,
    .deinit = pwm_capture_deinit,
    .reset = pwm_capture_reset,
    .get = pwm_capture_get,
    .set = pwm_capture_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = pwm_capture_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_pwm_capture = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "pwm_capture",
    .family = "pwm_capture",
    .config = NULL,
    .properties = pwm_capture_properties,
    .property_count = sizeof(pwm_capture_properties) / sizeof(pwm_capture_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = pwm_capture_bindings,
    .binding_count = sizeof(pwm_capture_bindings) / sizeof(pwm_capture_bindings[0]),
    .state_size = sizeof(struct pwm_capture_state),
    .ops = &pwm_capture_ops,
};
