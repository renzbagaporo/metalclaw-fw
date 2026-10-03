// SPDX-License-Identifier: Apache-2.0 OR MIT
// The two engines behind hal/uart.h: the PL011 blocks (uart.c) and a PIO
// program pair (uart_pio.c). Each engine's UART starts with this header, so
// the public functions in uart.c serve the common fields and dispatch the
// rest.

#ifndef MCLW_RP2_UART_INTERNAL_H
#define MCLW_RP2_UART_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"
#include "metalclaw/hal/uart.h"

struct mclw_uart_engine {
    void (*close)(struct mclw_uart *uart);
    int (*set_baud)(struct mclw_uart *uart, uint32_t baud);
    int (*set_format)(struct mclw_uart *uart, const struct mclw_uart_format *format);
    size_t (*send)(struct mclw_uart *uart, const uint8_t *bytes, size_t len);
};

// `stats` is written by the engine under the critical section; `on_rx` and
// its context are swapped under it too.
struct mclw_uart {
    const struct mclw_uart_engine *engine;
    const void *owner;
    uint32_t baud;
    mclw_uart_rx_t on_rx;
    void *on_rx_ctx;
    struct mclw_uart_stats stats;
};

struct mclw_uart *mclw_uart_pio_open(uint8_t tx, uint8_t rx, uint32_t baud,
    const struct mclw_uart_format *format, const void *owner, int *err);

#endif
