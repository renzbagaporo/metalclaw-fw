// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/swd.h"
#include "metalclaw/hal/usb.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "dap.h"

enum {
    PROP_ENABLED,
    PROP_CLOCK_HZ,
    PROP_CONNECTED,
    PROP_TRANSFERS,
    PROP_ERRORS,
};

enum {
    BINDING_SWD,
    BINDING_DAP,
};

struct swd_probe_state {
    struct metalclaw_dap dap;
    struct metalclaw_usb_interface *usb;
};

static const struct metalclaw_property_desc probe_properties[] = {
    {
        .name = "enabled",
        .type = METALCLAW_TYPE_BOOL,
        .count = 1,
        .access = METALCLAW_ACCESS_RW,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    {
        .name = "clock_hz",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RW,
        .unit = "Hz",
        .min = METALCLAW_DAP_CLOCK_MIN_HZ,
        .max = METALCLAW_DAP_CLOCK_MAX_HZ,
    },
    {
        .name = "connected",
        .type = METALCLAW_TYPE_BOOL,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    {
        .name = "transfers",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "errors",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
};

static const struct metalclaw_usb_interface_desc dap_interface = {
    .name = "CMSIS-DAP v2",
};

static const struct metalclaw_binding_desc probe_bindings[] = {
    {
        .name = "swd",
        .kind = METALCLAW_BINDING_SWD_WIRE,
        .required = true,
        .usb = NULL,
    },
    {
        .name = "dap",
        .kind = METALCLAW_BINDING_USB_INTERFACE,
        .required = true,
        .usb = &dap_interface,
    },
};

static int probe_init(struct metalclaw_instance *inst, const struct metalclaw_binding *bindings) {
    struct swd_probe_state *state = inst->state;

    state->usb = bindings[BINDING_DAP].usb;
    metalclaw_dap_init(&state->dap, inst, bindings[BINDING_SWD].swd);
    return metalclaw_dap_enable(&state->dap, true);
}

static void probe_deinit(struct metalclaw_instance *inst) {
    struct swd_probe_state *state = inst->state;

    (void)metalclaw_dap_enable(&state->dap, false);
    state->usb = NULL;
}

static int probe_reset(struct metalclaw_instance *inst, bool factory) {
    struct swd_probe_state *state = inst->state;

    return metalclaw_dap_reset(&state->dap, factory);
}

static int probe_get(struct metalclaw_instance *inst, size_t property, struct metalclaw_value *out) {
    struct swd_probe_state *state = inst->state;

    switch (property) {
        case PROP_ENABLED:
            out->v[0] = state->dap.enabled ? 1 : 0;
            return 0;
        case PROP_CLOCK_HZ:
            out->v[0] = metalclaw_hal_swd_get_clock(state->dap.swd);
            return 0;
        case PROP_CONNECTED:
            out->v[0] = state->dap.connected ? 1 : 0;
            return 0;
        case PROP_TRANSFERS:
            out->v[0] = state->dap.transfers;
            return 0;
        case PROP_ERRORS:
            out->v[0] = state->dap.errors;
            return 0;
        default:
            return -EINVAL;
    }
}

static int probe_set(struct metalclaw_instance *inst, size_t property,
    const struct metalclaw_value *in) {
    struct swd_probe_state *state = inst->state;

    switch (property) {
        case PROP_ENABLED:
            return metalclaw_dap_enable(&state->dap, in->v[0] != 0);
        case PROP_CLOCK_HZ:
            return metalclaw_dap_set_clock(&state->dap, (uint32_t)in->v[0]);
        default:
            return -EINVAL;
    }
}

static void probe_usb_rx(struct metalclaw_instance *inst, size_t binding, const uint8_t *data,
    size_t len) {
    struct swd_probe_state *state = inst->state;
    size_t response_length;

    if (binding != (size_t)BINDING_DAP || state->usb == NULL) {
        return;
    }
    response_length = metalclaw_dap_process(&state->dap, data, len);
    if (response_length != 0u) {
        (void)metalclaw_hal_usb_write(state->usb, state->dap.response, response_length);
    }
}

static const struct metalclaw_model_ops probe_ops = {
    .init = probe_init,
    .deinit = probe_deinit,
    .reset = probe_reset,
    .get = probe_get,
    .set = probe_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .usb_rx = probe_usb_rx,
};

const struct metalclaw_model metalclaw_model_swd_probe = {
    .abi_version = METALCLAW_ABI_VERSION,
    .id = "swd_probe",
    .family = "probe",
    .config = NULL,
    .properties = probe_properties,
    .property_count = sizeof(probe_properties) / sizeof(probe_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = probe_bindings,
    .binding_count = sizeof(probe_bindings) / sizeof(probe_bindings[0]),
    .state_size = sizeof(struct swd_probe_state),
    .ops = &probe_ops,
};
