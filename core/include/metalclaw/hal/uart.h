// SPDX-License-Identifier: Apache-2.0 OR MIT
// A UART the device speaks on, on whichever pin pair the user named. Sending
// queues bytes and returns; receiving delivers whatever has accumulated to
// `on_rx` on the worker under the runtime lock, never per byte and never from
// an interrupt, so a model may parse, touch its state, and answer with send()
// from inside the callback.

#ifndef MCLW_HAL_UART_H
#define MCLW_HAL_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"

struct mclw_uart;

// `overruns` is every received byte lost, the sum of the two below:
// `fifo_overruns` the receiver's own FIFO was full (a PL011's OE, a PIO
// receiver stalled), `ring_overruns` the port's receive ring was.
struct mclw_uart_stats {
    uint32_t bytes_rx;
    uint32_t bytes_tx;
    uint32_t overruns;
    uint32_t frame_errors;
    uint32_t fifo_overruns;
    uint32_t ring_overruns;
    // Breaks received: the line at its space level for a whole frame or
    // longer, counted apart from frame_errors.
    uint32_t breaks;
};

typedef void (*mclw_uart_rx_t)(struct mclw_uart *uart, const uint8_t *bytes, size_t len, void *ctx);
// A break received (a LIN header's 13 bits dominant), on the worker under
// the runtime lock as on_rx is, in order with the bytes around it: on_rx
// has had every byte before it, and gets the ones after.
typedef void (*mclw_uart_break_t)(struct mclw_uart *uart, void *ctx);

// Claims tx and rx for `owner`. NULL with *err set when the pins are taken,
// the port has no engine left for them, or cannot frame `format` on them.
struct mclw_uart *mclw_hal_uart_open(uint8_t tx, uint8_t rx, uint32_t baud,
    const struct mclw_uart_format *format, const void *owner, int *err);
void mclw_hal_uart_close(struct mclw_uart *uart);

// Both changes let what is already queued leave at the old setting first.
// -ENOTSUP for a format the port cannot frame on these pins.
int mclw_hal_uart_set_baud(struct mclw_uart *uart, uint32_t baud);
uint32_t mclw_hal_uart_baud(const struct mclw_uart *uart);
int mclw_hal_uart_set_format(struct mclw_uart *uart, const struct mclw_uart_format *format);
void mclw_hal_uart_set_rx(struct mclw_uart *uart, mclw_uart_rx_t fn, void *ctx);
void mclw_hal_uart_set_break(struct mclw_uart *uart, mclw_uart_break_t fn, void *ctx);
// Holds TX at its space level for `bits` bit times once what is queued has
// left, then lets it go; returns once it has. -ENOTSUP where the engine
// cannot.
int mclw_hal_uart_send_break(struct mclw_uart *uart, uint32_t bits);
// Holds TX at its space level from once what is queued has left (`on`)
// until it is let go (`on` false). -ENOTSUP where the engine cannot.
int mclw_hal_uart_hold_break(struct mclw_uart *uart, bool on);

// Returns how many bytes were queued; fewer than `len` when the queue is full.
size_t mclw_hal_uart_send(struct mclw_uart *uart, const uint8_t *bytes, size_t len);
void mclw_hal_uart_stats(const struct mclw_uart *uart, struct mclw_uart_stats *out);

#endif
