// SPDX-License-Identifier: Apache-2.0 OR MIT
// UARTs for the host port, served from the bridge. The DUT's transmitted
// bytes arrive from the emulator with the time they finished and go to
// the model's on_rx on the worker under the runtime lock, as on the rp2
// port; a model's send() becomes uart actions toward the DUT, one per
// byte, each at the time it would finish at the UART's baud rate and
// frame, after the bytes queued before it.

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/uart.h"

#include "host.h"

#define UARTS_MAX 4u
#define RX_RING 4096u

struct mclw_uart {
    const void *owner;
    uint8_t tx;
    uint8_t rx;
    uint32_t baud;
    struct mclw_uart_format format;
    mclw_uart_rx_t on_rx;
    void *on_rx_ctx;
    // Received bytes the worker has yet to deliver.
    uint8_t ring[RX_RING];
    uint32_t head;
    uint32_t tail;
    // When the last byte sent toward the DUT finishes, in virtual ns.
    uint64_t tx_busy_until_ns;
    struct mclw_uart_stats stats;
    struct mclw_work work;
};

static struct mclw_uart uarts[UARTS_MAX];

static uint64_t frame_ns(const struct mclw_uart *u) {
    uint32_t bits = 1u + u->format.data_bits + u->format.stop_bits + (u->format.parity != MCLW_UART_PARITY_NONE);

    return u->baud != 0u ? (uint64_t)bits * 1000000000u / u->baud : 0u;
}

static void rx_deliver(struct mclw_work *work) {
    struct mclw_uart *u = work->ctx;
    uint8_t chunk[RX_RING];

    if (!mclw_hal_try_lock()) {
        mclw_hal_work_post(work);
        return;
    }
    for (;;) {
        uint32_t state = mclw_hal_critical_enter();
        uint32_t n = u->head - u->tail;

        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = u->ring[(u->tail + i) % RX_RING];
        }
        u->tail += n;
        mclw_hal_critical_exit(state);
        if (n == 0u || u->owner == NULL || u->on_rx == NULL) {
            break;
        }
        u->on_rx(u, chunk, n, u->on_rx_ctx);
    }
    mclw_hal_unlock();
}

void mclw_host_uart_receive(uint8_t dut_tx_pin, const uint8_t *bytes, size_t len) {
    uint32_t state = mclw_hal_critical_enter();
    struct mclw_uart *found = NULL;

    for (size_t i = 0; i < UARTS_MAX && found == NULL; i++) {
        if (uarts[i].owner != NULL && uarts[i].rx == dut_tx_pin) {
            found = &uarts[i];
        }
    }
    if (found != NULL) {
        for (size_t i = 0; i < len; i++) {
            if (found->head - found->tail >= RX_RING) {
                found->stats.ring_overruns++;
                found->stats.overruns++;
            } else {
                found->ring[found->head++ % RX_RING] = bytes[i];
                found->stats.bytes_rx++;
            }
        }
    }
    mclw_hal_critical_exit(state);
    if (found != NULL) {
        mclw_hal_work_post(&found->work);
    }
}

struct mclw_uart *mclw_hal_uart_open(uint8_t tx, uint8_t rx, uint32_t baud,
    const struct mclw_uart_format *format, const void *owner, int *err) {
    struct mclw_uart *u = NULL;

    if (owner == NULL || format == NULL || baud == 0u) {
        *err = -EINVAL;
        return NULL;
    }
    uint32_t state = mclw_hal_critical_enter();
    for (size_t i = 0; i < UARTS_MAX && u == NULL; i++) {
        if (uarts[i].owner == NULL) {
            u = &uarts[i];
        }
    }
    mclw_hal_critical_exit(state);
    if (u == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    *err = mclw_hal_pin_claim(tx, owner);
    if (*err == 0 && rx != tx) {
        *err = mclw_hal_pin_claim(rx, owner);
        if (*err != 0) {
            mclw_hal_pin_release(tx, owner);
        }
    }
    if (*err != 0) {
        return NULL;
    }
    state = mclw_hal_critical_enter();
    memset(u, 0, sizeof(*u));
    u->tx = tx;
    u->rx = rx;
    u->baud = baud;
    u->format = *format;
    u->work.fn = rx_deliver;
    u->work.ctx = u;
    u->owner = owner;
    mclw_hal_critical_exit(state);
    return u;
}

void mclw_hal_uart_close(struct mclw_uart *u) {
    if (u == NULL || u->owner == NULL) {
        return;
    }
    const void *owner = u->owner;
    uint32_t state = mclw_hal_critical_enter();

    u->owner = NULL;
    u->on_rx = NULL;
    mclw_hal_critical_exit(state);
    mclw_hal_pin_release(u->tx, owner);
    mclw_hal_pin_release(u->rx, owner);
}

int mclw_hal_uart_set_baud(struct mclw_uart *u, uint32_t baud) {
    if (baud == 0u) {
        return -EINVAL;
    }
    u->baud = baud;
    return 0;
}

uint32_t mclw_hal_uart_baud(const struct mclw_uart *u) {
    return u->baud;
}

int mclw_hal_uart_set_format(struct mclw_uart *u, const struct mclw_uart_format *format) {
    u->format = *format;
    return 0;
}

void mclw_hal_uart_set_rx(struct mclw_uart *u, mclw_uart_rx_t fn, void *ctx) {
    uint32_t state = mclw_hal_critical_enter();

    u->on_rx = fn;
    u->on_rx_ctx = ctx;
    mclw_hal_critical_exit(state);
}

size_t mclw_hal_uart_send(struct mclw_uart *u, const uint8_t *bytes, size_t len) {
    uint64_t frame = frame_ns(u);
    uint32_t state = mclw_hal_critical_enter();
    uint64_t at = mclw_host_event_ns();

    if (u->tx_busy_until_ns > at) {
        at = u->tx_busy_until_ns;
    }
    for (size_t i = 0; i < len; i++) {
        at += frame;
        mclw_host_event_uart(u->tx, bytes[i], at);
    }
    u->tx_busy_until_ns = at;
    u->stats.bytes_tx += (uint32_t)len;
    mclw_hal_critical_exit(state);
    return len;
}

void mclw_hal_uart_stats(const struct mclw_uart *u, struct mclw_uart_stats *out) {
    uint32_t state = mclw_hal_critical_enter();

    *out = u->stats;
    mclw_hal_critical_exit(state);
}
