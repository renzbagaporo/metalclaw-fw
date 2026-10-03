// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"
#include "metalclaw/hal/uart.h"
#include "metalclaw/hal/usb.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "console_bridge.h"

enum {
    PROP_BAUD,
    PROP_FOLLOW_LINE_CODING,
    PROP_DTR,
    PROP_RTS,
    PROP_TO_HOST,
    PROP_FROM_HOST,
    PROP_OVERRUNS,
    PROP_DISCARDED,
    PROP_CODINGS_REFUSED,
};

enum {
    BINDING_UART,
    BINDING_USB,
};

struct console_bridge_state {
    struct mclw_instance *inst;
    struct mclw_uart *uart;
    struct mclw_usb_interface *usb;
    // What was asked of the UART last; the engine reports the rate it
    // achieved, which the host's coding is not compared against.
    uint32_t baud;
    struct mclw_uart_format format;
    bool follow_line_coding;
    uint32_t to_host;
    uint32_t from_host;
    uint32_t overruns;
    uint32_t discarded;
    uint32_t codings_refused;
};

#define COUNTER(prop_name) \
    { .name = prop_name, .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, \
      .unit = "bytes", .min = 0, .max = UINT32_MAX }

static const struct mclw_property_desc bridge_properties[] = {
    {
        .name = "baud",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "baud",
        .min = CONSOLE_BRIDGE_BAUD_MIN,
        .max = CONSOLE_BRIDGE_BAUD_MAX,
    },
    {
        .name = "follow_line_coding",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    {
        .name = "dtr",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    {
        .name = "rts",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    COUNTER("to_host"),
    COUNTER("from_host"),
    COUNTER("overruns"),
    COUNTER("discarded"),
    {
        .name = "codings_refused",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
};

static const struct mclw_usb_interface_desc console_interface = {
    .name = CONSOLE_BRIDGE_INTERFACE,
    .function = MCLW_USB_SERIAL,
};

static const struct mclw_binding_desc bridge_bindings[] = {
    {
        .name = "uart",
        .kind = MCLW_BINDING_UART,
        .required = true,
        .usb = NULL,
        .uart = {
            .default_baud = CONSOLE_BRIDGE_DEFAULT_BAUD,
            .format = {
                .data_bits = 8,
                .stop_bits = 1,
                .parity = MCLW_UART_PARITY_NONE,
            },
        },
    },
    {
        .name = "usb",
        .kind = MCLW_BINDING_USB_INTERFACE,
        .required = true,
        .usb = &console_interface,
    },
};

static bool same_format(const struct mclw_uart_format *a, const struct mclw_uart_format *b) {
    return a->data_bits == b->data_bits && a->stop_bits == b->stop_bits && a->parity == b->parity;
}

// Takes the host's line coding onto the UART. The format goes first, since
// it is what an engine refuses; a coding the UART cannot take is counted
// and leaves the UART as it was.
static void apply_line_coding(struct console_bridge_state *state) {
    struct mclw_usb_line_coding coding;
    struct mclw_uart_format previous = state->format;
    int err = mclw_hal_usb_line_coding(state->usb, &coding);

    if (err == 0 && (coding.baud < CONSOLE_BRIDGE_BAUD_MIN || coding.baud > CONSOLE_BRIDGE_BAUD_MAX)) {
        err = -EINVAL;
    }
    if (err == 0 && !same_format(&coding.format, &state->format)) {
        err = mclw_hal_uart_set_format(state->uart, &coding.format);
        if (err == 0) {
            state->format = coding.format;
        }
    }
    if (err == 0 && coding.baud != state->baud) {
        err = mclw_hal_uart_set_baud(state->uart, coding.baud);
        if (err == 0) {
            state->baud = coding.baud;
        } else if (!same_format(&previous, &state->format) &&
            mclw_hal_uart_set_format(state->uart, &previous) == 0) {
            state->format = previous;
        }
    }
    if (err != 0) {
        state->codings_refused++;
    }
    mclw_trace(state->inst, MCLW_TRACE_STATE, state->baud, 0, (int16_t)err);
}

// The worker hands over what the DUT sent; whatever the host queue cannot
// take is lost, an overrun while the host has the port open.
static void on_rx(struct mclw_uart *uart, const uint8_t *bytes, size_t len, void *ctx) {
    struct console_bridge_state *state = ctx;
    size_t queued = mclw_hal_usb_write(state->usb, bytes, len);

    (void)uart;
    state->to_host += (uint32_t)queued;
    if (queued < len) {
        if (mclw_hal_usb_connected(state->usb)) {
            state->overruns += (uint32_t)(len - queued);
        } else {
            state->discarded += (uint32_t)(len - queued);
        }
    }
}

static void restore_defaults(struct console_bridge_state *state) {
    const struct mclw_uart_facts *facts = &bridge_bindings[BINDING_UART].uart;

    if (state->baud != facts->default_baud && mclw_hal_uart_set_baud(state->uart, facts->default_baud) == 0) {
        state->baud = facts->default_baud;
    }
    if (!same_format(&state->format, &facts->format) &&
        mclw_hal_uart_set_format(state->uart, &facts->format) == 0) {
        state->format = facts->format;
    }
    state->follow_line_coding = false;
}

static int bridge_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct console_bridge_state *state = inst->state;

    state->inst = inst;
    state->uart = bindings[BINDING_UART].uart;
    state->usb = bindings[BINDING_USB].usb;
    state->baud = bridge_bindings[BINDING_UART].uart.default_baud;
    state->format = bridge_bindings[BINDING_UART].uart.format;
    mclw_hal_uart_set_rx(state->uart, on_rx, state);
    return 0;
}

static void bridge_deinit(struct mclw_instance *inst) {
    struct console_bridge_state *state = inst->state;

    mclw_hal_uart_set_rx(state->uart, NULL, NULL);
}

static int bridge_reset(struct mclw_instance *inst, bool factory) {
    struct console_bridge_state *state = inst->state;

    if (factory) {
        restore_defaults(state);
    }
    state->to_host = 0;
    state->from_host = 0;
    state->overruns = 0;
    state->discarded = 0;
    state->codings_refused = 0;
    return 0;
}

static int bridge_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct console_bridge_state *state = inst->state;
    uint8_t lines = mclw_hal_usb_control_lines(state->usb);

    switch (property) {
        case PROP_BAUD:
            out->v[0] = state->baud;
            return 0;
        case PROP_FOLLOW_LINE_CODING:
            out->v[0] = state->follow_line_coding ? 1 : 0;
            return 0;
        case PROP_DTR:
            out->v[0] = (lines & MCLW_USB_LINE_DTR) != 0u ? 1 : 0;
            return 0;
        case PROP_RTS:
            out->v[0] = (lines & MCLW_USB_LINE_RTS) != 0u ? 1 : 0;
            return 0;
        case PROP_TO_HOST:
            out->v[0] = state->to_host;
            return 0;
        case PROP_FROM_HOST:
            out->v[0] = state->from_host;
            return 0;
        case PROP_OVERRUNS:
            out->v[0] = state->overruns;
            return 0;
        case PROP_DISCARDED:
            out->v[0] = state->discarded;
            return 0;
        case PROP_CODINGS_REFUSED:
            out->v[0] = state->codings_refused;
            return 0;
        default:
            return -EINVAL;
    }
}

static int bridge_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct console_bridge_state *state = inst->state;

    switch (property) {
        case PROP_BAUD: {
            int err = mclw_hal_uart_set_baud(state->uart, (uint32_t)in->v[0]);
            if (err == 0) {
                state->baud = (uint32_t)in->v[0];
            }
            return err;
        }
        case PROP_FOLLOW_LINE_CODING:
            state->follow_line_coding = in->v[0] != 0;
            if (state->follow_line_coding) {
                apply_line_coding(state);
            }
            return 0;
        default:
            return -EINVAL;
    }
}

// The host's bytes go to the UART as far as its queue takes them; the rest
// stays with the USB function, which holds the host off until it does.
static size_t bridge_usb_rx(struct mclw_instance *inst, size_t binding, const uint8_t *data, size_t len) {
    struct console_bridge_state *state = inst->state;

    if (binding != (size_t)BINDING_USB) {
        return len;
    }
    size_t sent = mclw_hal_uart_send(state->uart, data, len);
    state->from_host += (uint32_t)sent;
    return sent;
}

// DTR and RTS are only reported: on this bridge they reset nothing.
static void bridge_usb_control(struct mclw_instance *inst, size_t binding) {
    struct console_bridge_state *state = inst->state;

    if (binding == (size_t)BINDING_USB && state->follow_line_coding) {
        apply_line_coding(state);
    }
}

static const struct mclw_model_ops bridge_ops = {
    .init = bridge_init,
    .deinit = bridge_deinit,
    .reset = bridge_reset,
    .get = bridge_get,
    .set = bridge_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .usb_rx = bridge_usb_rx,
    .usb_control = bridge_usb_control,
};

const struct mclw_model mclw_model_console_bridge = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "console_bridge",
    .family = "bridge",
    .config = NULL,
    .properties = bridge_properties,
    .property_count = sizeof(bridge_properties) / sizeof(bridge_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = bridge_bindings,
    .binding_count = sizeof(bridge_bindings) / sizeof(bridge_bindings[0]),
    .state_size = sizeof(struct console_bridge_state),
    .ops = &bridge_ops,
};
