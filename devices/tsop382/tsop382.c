// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/wire.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "nec.h"
#include "tsop382.h"

enum {
    PROP_BURSTS,
    PROP_FRAMES,
    PROP_REPEATS,
    PROP_ERRORS,
    PROP_LAST,
    PROP_ADDRESS,
    PROP_COMMAND,
};

enum {
    BINDING_IR,
};

#define READ_BATCH 16u

struct tsop382_state {
    struct mclw_wire *ir;
    uint32_t bursts;
    struct nec_decoder nec;
};

#define COUNTER(property_name) \
    { \
        .name = (property_name), \
        .type = MCLW_TYPE_U32, \
        .count = 1, \
        .access = MCLW_ACCESS_RO, \
        .unit = NULL, \
        .min = 0, \
        .max = UINT32_MAX, \
    }

static const struct mclw_property_desc tsop382_properties[] = {
    COUNTER("bursts"),
    COUNTER("frames"),
    COUNTER("repeats"),
    COUNTER("errors"),
    COUNTER("last"),
    {
        .name = "address",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 0xff,
    },
    {
        .name = "command",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 0xff,
    },
};

// TSOP382 (Vishay 82491 Rev. 2.1) table "Suitable data format": after each
// burst a gap of at least 12 carrier cycles; a burst is at least 10. Two
// carrier periods at 38 kHz without an edge end a burst here: longer than
// the carrier's low half, much shorter than any gap the part needs.
static const struct mclw_binding_desc tsop382_bindings[] = {
    {
        .name = "ir",
        .kind = MCLW_BINDING_WIRE,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
        .wire = { .engine = MCLW_WIRE_ENVELOPE, .pins = 2, .envelope = { .gap_ns = 52000 } },
    },
};

static void tsop382_poll(struct mclw_instance *inst) {
    struct tsop382_state *state = inst->state;
    uint32_t entries[READ_BATCH];
    size_t n;

    while ((n = mclw_hal_wire_read(state->ir, entries, READ_BATCH)) > 0u) {
        for (size_t i = 0; i < n; i++) {
            bool mark = (entries[i] & MCLW_WIRE_MARK) != 0u;
            if (mark) {
                state->bursts++;
            }
            if (nec_take(&state->nec, mark, entries[i] & ~MCLW_WIRE_MARK)) {
                mclw_trace(inst, MCLW_TRACE_STATE, state->nec.last, 4, 0);
            }
        }
    }
}

static int tsop382_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct tsop382_state *state = inst->state;

    state->ir = bindings[BINDING_IR].wire;
    state->bursts = 0;
    nec_init(&state->nec);
    return 0;
}

static void tsop382_deinit(struct mclw_instance *inst) {
    struct tsop382_state *state = inst->state;

    state->ir = NULL;
}

static int tsop382_reset(struct mclw_instance *inst, bool factory) {
    struct tsop382_state *state = inst->state;

    (void)factory;
    state->bursts = 0;
    nec_init(&state->nec);
    return 0;
}

static int tsop382_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct tsop382_state *state = inst->state;

    switch (property) {
        case PROP_BURSTS:
            out->v[0] = state->bursts;
            return 0;
        case PROP_FRAMES:
            out->v[0] = state->nec.frames;
            return 0;
        case PROP_REPEATS:
            out->v[0] = state->nec.repeats;
            return 0;
        case PROP_ERRORS:
            out->v[0] = state->nec.errors;
            return 0;
        case PROP_LAST:
            out->v[0] = state->nec.last;
            return 0;
        case PROP_ADDRESS:
            out->v[0] = state->nec.last & 0xffu;
            return 0;
        case PROP_COMMAND:
            out->v[0] = (state->nec.last >> 16) & 0xffu;
            return 0;
        default:
            return -EINVAL;
    }
}

static const struct mclw_model_ops tsop382_ops = {
    .init = tsop382_init,
    .deinit = tsop382_deinit,
    .reset = tsop382_reset,
    .get = tsop382_get,
    .set = NULL,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = tsop382_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_tsop38238 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "tsop38238",
    .family = "tsop382",
    .config = NULL,
    .properties = tsop382_properties,
    .property_count = sizeof(tsop382_properties) / sizeof(tsop382_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = tsop382_bindings,
    .binding_count = sizeof(tsop382_bindings) / sizeof(tsop382_bindings[0]),
    .state_size = sizeof(struct tsop382_state),
    .ops = &tsop382_ops,
};
