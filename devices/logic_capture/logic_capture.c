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

#include "logic_capture.h"

enum {
    PROP_LEVEL,
    PROP_RISING,
    PROP_FALLING,
    PROP_EDGES,
    PROP_MISSED,
    PROP_LAST_EDGE_MS,
    PROP_HIGH_NS,
    PROP_LOW_NS,
};

enum {
    BINDING_PIN,
};

struct logic_capture_state {
    struct mclw_wire *pin;
    struct mclw_edges edges;
};

#define COUNTER(property_name, property_unit) \
    { \
        .name = (property_name), \
        .type = MCLW_TYPE_U32, \
        .count = 1, \
        .access = MCLW_ACCESS_RO, \
        .unit = (property_unit), \
        .min = 0, \
        .max = UINT32_MAX, \
    }

static const struct mclw_property_desc logic_capture_properties[] = {
    {
        .name = "level",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = -1,
        .max = 1,
    },
    COUNTER("rising", NULL),
    COUNTER("falling", NULL),
    COUNTER("edges", NULL),
    COUNTER("missed", NULL),
    COUNTER("last_edge_ms", "ms"),
    COUNTER("high_ns", "ns"),
    COUNTER("low_ns", "ns"),
};

static const struct mclw_binding_desc logic_capture_bindings[] = {
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

static void logic_capture_poll(struct mclw_instance *inst) {
    struct logic_capture_state *state = inst->state;
    uint32_t edges = state->edges.rising + state->edges.falling;

    mclw_edges_poll(&state->edges, state->pin, mclw_now_us());
    if (state->edges.rising + state->edges.falling != edges) {
        mclw_trace(inst, MCLW_TRACE_STATE, state->edges.rising, (uint16_t)state->edges.falling,
            (int16_t)state->edges.level);
    }
}

static int logic_capture_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct logic_capture_state *state = inst->state;

    state->pin = bindings[BINDING_PIN].wire;
    mclw_edges_init(&state->edges);
    return 0;
}

static void logic_capture_deinit(struct mclw_instance *inst) {
    struct logic_capture_state *state = inst->state;

    state->pin = NULL;
}

// The counts start again; the level and what the engine has still to
// hand over are kept.
static int logic_capture_reset(struct mclw_instance *inst, bool factory) {
    struct logic_capture_state *state = inst->state;
    int8_t level = state->edges.level;
    uint32_t overruns = state->edges.overruns_seen;

    (void)factory;
    mclw_edges_init(&state->edges);
    state->edges.level = level;
    state->edges.overruns_seen = overruns;
    return 0;
}

static int logic_capture_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct logic_capture_state *state = inst->state;
    const struct mclw_edges *e = &state->edges;

    switch (property) {
        case PROP_LEVEL:
            out->v[0] = e->level;
            return 0;
        case PROP_RISING:
            out->v[0] = e->rising;
            return 0;
        case PROP_FALLING:
            out->v[0] = e->falling;
            return 0;
        case PROP_EDGES:
            out->v[0] = (uint32_t)(e->rising + e->falling);
            return 0;
        case PROP_MISSED:
            out->v[0] = e->missed;
            return 0;
        case PROP_LAST_EDGE_MS:
            out->v[0] = e->any_edge ? (uint32_t)(e->last_edge_us / 1000u) : 0u;
            return 0;
        case PROP_HIGH_NS:
            out->v[0] = e->high_ns;
            return 0;
        case PROP_LOW_NS:
            out->v[0] = e->low_ns;
            return 0;
        default:
            return -EINVAL;
    }
}

static const struct mclw_model_ops logic_capture_ops = {
    .init = logic_capture_init,
    .deinit = logic_capture_deinit,
    .reset = logic_capture_reset,
    .get = logic_capture_get,
    .set = NULL,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = logic_capture_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_logic_capture = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "logic_capture",
    .family = "logic_capture",
    .config = NULL,
    .properties = logic_capture_properties,
    .property_count = sizeof(logic_capture_properties) / sizeof(logic_capture_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = logic_capture_bindings,
    .binding_count = sizeof(logic_capture_bindings) / sizeof(logic_capture_bindings[0]),
    .state_size = sizeof(struct logic_capture_state),
    .ops = &logic_capture_ops,
};
