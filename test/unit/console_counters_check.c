// SPDX-License-Identifier: Apache-2.0 OR MIT
// The receive counters of a console's path on the host: the rp2 UART
// engines' per-word count (uart_internal.h), and the console_bridge model's
// hand-off to a fake USB write that takes only as many bytes as it is told.
//
//   uart <words>   each word one received byte's flags: '.' clean, 'o' the
//                  receiver overflowed after it, 'f' a framing error, 'r'
//                  no room in the ring, 'b' both 'o' and 'r'
//   bridge <takes...>  each take one hand-off of 8 bytes, the write taking
//                  that many ("all" for every one), with DTR held; "B" a
//                  break from the DUT instead, "S<ms>" a SEND_BREAK from the
//                  host
//
// Prints the counters as key=value on one line.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metalclaw/hal/uart.h"
#include "metalclaw/hal/usb.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "uart_internal.h"

extern const struct mclw_model mclw_model_console_bridge;

static size_t write_takes;
static mclw_uart_rx_t bridge_rx;
static void *bridge_rx_ctx;
static mclw_uart_break_t bridge_break;
static void *bridge_break_ctx;
static unsigned usb_breaks;
static uint16_t host_break_ms;
static uint32_t uart_break_bits;

int mclw_hal_usb_serial_break(struct mclw_usb_interface *itf) {
    (void)itf;
    usb_breaks++;
    return 0;
}

uint16_t mclw_hal_usb_serial_take_break(struct mclw_usb_interface *itf) {
    uint16_t ms = host_break_ms;

    (void)itf;
    host_break_ms = 0;
    return ms;
}

void mclw_hal_uart_set_break(struct mclw_uart *uart, mclw_uart_break_t fn, void *ctx) {
    (void)uart;
    bridge_break = fn;
    bridge_break_ctx = ctx;
}

int mclw_hal_uart_send_break(struct mclw_uart *uart, uint32_t bits) {
    (void)uart;
    uart_break_bits = bits;
    return 0;
}

size_t mclw_hal_usb_write(struct mclw_usb_interface *itf, const uint8_t *data, size_t len) {
    (void)itf;
    (void)data;
    return len < write_takes ? len : write_takes;
}

bool mclw_hal_usb_connected(const struct mclw_usb_interface *itf) {
    (void)itf;
    return true;
}

int mclw_hal_usb_line_coding(const struct mclw_usb_interface *itf, struct mclw_usb_line_coding *out) {
    (void)itf;
    (void)out;
    return -1;
}

uint8_t mclw_hal_usb_control_lines(const struct mclw_usb_interface *itf) {
    (void)itf;
    return MCLW_USB_LINE_DTR;
}

void mclw_hal_uart_set_rx(struct mclw_uart *uart, mclw_uart_rx_t fn, void *ctx) {
    (void)uart;
    bridge_rx = fn;
    bridge_rx_ctx = ctx;
}

int mclw_hal_uart_set_baud(struct mclw_uart *uart, uint32_t baud) {
    (void)uart;
    (void)baud;
    return 0;
}

int mclw_hal_uart_set_format(struct mclw_uart *uart, const struct mclw_uart_format *format) {
    (void)uart;
    (void)format;
    return 0;
}

size_t mclw_hal_uart_send(struct mclw_uart *uart, const uint8_t *bytes, size_t len) {
    (void)uart;
    (void)bytes;
    return len;
}

void mclw_trace(struct mclw_instance *inst, enum mclw_trace_kind kind,
    uint32_t addr, uint16_t len, int16_t result) {
    (void)inst;
    (void)kind;
    (void)addr;
    (void)len;
    (void)result;
}

static int property(const char *name) {
    for (size_t i = 0; i < mclw_model_console_bridge.property_count; i++) {
        if (strcmp(mclw_model_console_bridge.properties[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static long long get(struct mclw_instance *inst, const char *name) {
    struct mclw_value value = { .count = 1 };
    int index = property(name);

    if (index < 0 || mclw_model_console_bridge.ops->get(inst, (size_t)index, &value) != 0) {
        return -1;
    }
    return (long long)value.v[0];
}

static int uart(const char *words) {
    struct mclw_uart_stats st;
    unsigned ringed = 0;

    memset(&st, 0, sizeof(st));
    for (const char *w = words; *w != '\0'; w++) {
        bool overrun = *w == 'o' || *w == 'b';

        if (mclw_uart_rx_count(&st, overrun, *w == 'f', *w == 'r' || *w == 'b')) {
            ringed++;
        }
    }
    printf("ringed=%u bytes_rx=%u overruns=%u fifo_overruns=%u ring_overruns=%u frame_errors=%u\n", ringed,
        (unsigned)st.bytes_rx, (unsigned)st.overruns, (unsigned)st.fifo_overruns, (unsigned)st.ring_overruns,
        (unsigned)st.frame_errors);
    return 0;
}

static int bridge(int argc, char **argv) {
    static const uint8_t bytes[8] = { 0 };
    static max_align_t state[32];
    static max_align_t fake_uart;
    static max_align_t fake_interface;
    struct mclw_instance inst;
    struct mclw_binding bindings[2];

    if (mclw_model_console_bridge.state_size > sizeof(state)) {
        return 2;
    }
    memset(&inst, 0, sizeof(inst));
    memset(bindings, 0, sizeof(bindings));
    inst.model = &mclw_model_console_bridge;
    inst.state = state;
    bindings[0].kind = MCLW_BINDING_UART;
    bindings[0].present = true;
    bindings[0].uart = (struct mclw_uart *)(void *)&fake_uart;
    bindings[1].kind = MCLW_BINDING_USB_INTERFACE;
    bindings[1].present = true;
    bindings[1].usb = (struct mclw_usb_interface *)(void *)&fake_interface;
    if (mclw_model_console_bridge.ops->init(&inst, bindings) != 0 || bridge_rx == NULL) {
        return 3;
    }
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "B") == 0) {
            bridge_break(bindings[0].uart, bridge_break_ctx);
            continue;
        }
        if (argv[i][0] == 'S') {
            host_break_ms = (uint16_t)strtoul(argv[i] + 1, NULL, 0);
            mclw_model_console_bridge.ops->usb_control(&inst, 1);
            mclw_model_console_bridge.ops->time_advanced(&inst, 0);
            continue;
        }
        write_takes = strcmp(argv[i], "all") == 0 ? SIZE_MAX : (size_t)strtoul(argv[i], NULL, 0);
        bridge_rx(bindings[0].uart, bytes, sizeof(bytes), bridge_rx_ctx);
    }
    printf("to_host=%lld overruns=%lld short_writes=%lld breaks_to_host=%lld usb_breaks=%u breaks_from_host=%lld "
        "break_bits=%u", get(&inst, "to_host"), get(&inst, "overruns"), get(&inst, "short_writes"),
        get(&inst, "breaks_to_host"), usb_breaks, get(&inst, "breaks_from_host"), (unsigned)uart_break_bits);
    if (mclw_model_console_bridge.ops->reset(&inst, false) != 0) {
        return 4;
    }
    printf(" reset_short_writes=%lld\n", get(&inst, "short_writes"));
    mclw_model_console_bridge.ops->deinit(&inst);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "uart") == 0) {
        return uart(argv[2]);
    }
    if (argc >= 2 && strcmp(argv[1], "bridge") == 0) {
        return bridge(argc - 2, argv + 2);
    }
    return 1;
}
