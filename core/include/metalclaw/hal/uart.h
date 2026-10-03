// SPDX-License-Identifier: Apache-2.0 OR MIT
// A UART the device speaks on. Sending queues bytes and returns; receiving
// delivers whatever has accumulated to `on_rx` on the worker under the
// runtime lock, never per byte and never from an interrupt, so a model may
// parse, touch its state, and answer with send() from inside the callback.

#ifndef MCLW_HAL_UART_H
#define MCLW_HAL_UART_H

#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"

struct mclw_uart;

struct mclw_uart_stats {
    uint32_t bytes_rx;
    uint32_t bytes_tx;
    uint32_t overruns;
    uint32_t frame_errors;
};

typedef void (*mclw_uart_rx_t)(struct mclw_uart *uart, const uint8_t *bytes, size_t len, void *ctx);

// Claims tx and rx for `owner`. NULL with *err set when the pins are not a
// UART pair on this port or are taken.
struct mclw_uart *mclw_hal_uart_open(uint8_t tx, uint8_t rx, uint32_t baud,
    const struct mclw_uart_format *format, const void *owner, int *err);
void mclw_hal_uart_close(struct mclw_uart *uart);

// Both changes let what is already queued leave at the old setting first.
int mclw_hal_uart_set_baud(struct mclw_uart *uart, uint32_t baud);
uint32_t mclw_hal_uart_baud(const struct mclw_uart *uart);
int mclw_hal_uart_set_format(struct mclw_uart *uart, const struct mclw_uart_format *format);
void mclw_hal_uart_set_rx(struct mclw_uart *uart, mclw_uart_rx_t fn, void *ctx);

// Returns how many bytes were queued; fewer than `len` when the queue is full.
size_t mclw_hal_uart_send(struct mclw_uart *uart, const uint8_t *bytes, size_t len);
void mclw_hal_uart_stats(const struct mclw_uart *uart, struct mclw_uart_stats *out);

#endif
