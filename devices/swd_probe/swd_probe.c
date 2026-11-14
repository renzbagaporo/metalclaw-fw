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
    PROP_SHORT_REPLIES,
};

enum {
    BINDING_SWD,
    BINDING_DAP,
};

struct swd_probe_state {
    struct mclw_dap dap;
    struct mclw_usb_interface *usb;
    uint32_t short_replies;
};

static const struct mclw_property_desc probe_properties[] = {
    {
        .name = "enabled",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    {
        .name = "clock_hz",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "Hz",
        .min = MCLW_DAP_CLOCK_MIN_HZ,
        .max = MCLW_DAP_CLOCK_MAX_HZ,
    },
    {
        .name = "connected",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    {
        .name = "transfers",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "errors",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "short_replies",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
};

static const struct mclw_usb_interface_desc dap_interface = {
    .name = "CMSIS-DAP v2",
    .function = MCLW_USB_VENDOR,
};

static const struct mclw_binding_desc probe_bindings[] = {
    {
        .name = "swd",
        .kind = MCLW_BINDING_SWD_WIRE,
        .required = true,
        .usb = NULL,
    },
    {
        .name = "dap",
        .kind = MCLW_BINDING_USB_INTERFACE,
        .required = true,
        .usb = &dap_interface,
    },
};

static int probe_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct swd_probe_state *state = inst->state;

    state->usb = bindings[BINDING_DAP].usb;
    mclw_dap_init(&state->dap, inst, bindings[BINDING_SWD].swd);
    return mclw_dap_enable(&state->dap, true);
}

static void probe_deinit(struct mclw_instance *inst) {
    struct swd_probe_state *state = inst->state;

    (void)mclw_dap_enable(&state->dap, false);
    state->usb = NULL;
}

static int probe_reset(struct mclw_instance *inst, bool factory) {
    struct swd_probe_state *state = inst->state;

    state->short_replies = 0u;
    return mclw_dap_reset(&state->dap, factory);
}

static int probe_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct swd_probe_state *state = inst->state;

    switch (property) {
        case PROP_ENABLED:
            out->v[0] = state->dap.enabled ? 1 : 0;
            return 0;
        case PROP_CLOCK_HZ:
            out->v[0] = mclw_hal_swd_get_clock(state->dap.swd);
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
        case PROP_SHORT_REPLIES:
            out->v[0] = state->short_replies;
            return 0;
        default:
            return -EINVAL;
    }
}

static int probe_set(struct mclw_instance *inst, size_t property,
    const struct mclw_value *in) {
    struct swd_probe_state *state = inst->state;

    switch (property) {
        case PROP_ENABLED:
            return mclw_dap_enable(&state->dap, in->v[0] != 0);
        case PROP_CLOCK_HZ:
            return mclw_dap_set_clock(&state->dap, (uint32_t)in->v[0]);
        default:
            return -EINVAL;
    }
}

static size_t probe_usb_rx(struct mclw_instance *inst, size_t binding, const uint8_t *data,
    size_t len) {
    struct swd_probe_state *state = inst->state;
    size_t response_length;

    if (binding != (size_t)BINDING_DAP || state->usb == NULL) {
        return len;
    }
    response_length = mclw_dap_process(&state->dap, data, len);
    // The interface keeps what its TX FIFO refuses in a backlog; a reply it
    // still cannot take whole finds both full, and waiting here would not
    // help, as both drain only from the USB task this op runs in.
    if (response_length != 0u &&
        mclw_hal_usb_write(state->usb, state->dap.response, response_length) != response_length) {
        state->short_replies++;
    }
    return len;
}

static const struct mclw_model_ops probe_ops = {
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
    .usb_control = NULL,
};

const struct mclw_model mclw_model_swd_probe = {
    .abi_version = MCLW_ABI_VERSION,
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
