// SPDX-License-Identifier: Apache-2.0 OR MIT
// A UART the device speaks on. Sending queues bytes and returns; receiving
// delivers whatever has accumulated to `on_rx` in thread context, never per
// byte and never from an interrupt, so a model may parse, allocate nothing,
// and answer with send() from inside the callback.

#ifndef METALCLAW_HAL_UART_H
#define METALCLAW_HAL_UART_H

#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"

struct metalclaw_uart;

struct metalclaw_uart_stats {
    uint32_t bytes_rx;
    uint32_t bytes_tx;
    uint32_t overruns;
    uint32_t frame_errors;
};

typedef void (*metalclaw_uart_rx_t)(struct metalclaw_uart *uart, const uint8_t *bytes, size_t len, void *ctx);

// Claims tx and rx for `owner`. NULL with *err set when the pins are not a
// UART pair on this port or are taken.
struct metalclaw_uart *metalclaw_hal_uart_open(uint8_t tx, uint8_t rx, uint32_t baud,
    const struct metalclaw_uart_format *format, const void *owner, int *err);
void metalclaw_hal_uart_close(struct metalclaw_uart *uart);

int metalclaw_hal_uart_set_baud(struct metalclaw_uart *uart, uint32_t baud);
uint32_t metalclaw_hal_uart_baud(const struct metalclaw_uart *uart);
void metalclaw_hal_uart_set_rx(struct metalclaw_uart *uart, metalclaw_uart_rx_t fn, void *ctx);

// Returns how many bytes were queued; fewer than `len` when the queue is full.
size_t metalclaw_hal_uart_send(struct metalclaw_uart *uart, const uint8_t *bytes, size_t len);
void metalclaw_hal_uart_stats(const struct metalclaw_uart *uart, struct metalclaw_uart_stats *out);

#endif
