// SPDX-License-Identifier: Apache-2.0 OR MIT
// UARTs for the host port, served from the bridge. The DUT's transmitted
// bytes arrive from the emulator with the time they finished and go to
// the model's on_rx on the worker under the runtime lock, as on the rp2
// port; a model's send() becomes uart actions toward the DUT, one per
// byte, each at the time it would finish at the UART's baud rate and
// frame, after the bytes queued before it. On pins no UART block of the
// DUT's has (a PIO program's UART), the line goes bit by bit instead: the
// DUT's TX is decoded from the edges the bridge reports, sampled mid-bit,
// and bytes toward the DUT go as timed level actions on its RX pin.

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/uart.h"

#include "host.h"

#define UARTS_MAX 4u
#define RX_RING 4096u
// Edges kept for one frame: start, data, parity and stop bits.
#define FRAME_EDGES 16u
#define UART_BREAKS_MAX 8u

struct mclw_uart {
    const void *owner;
    uint8_t tx;
    uint8_t rx;
    uint32_t baud;
    struct mclw_uart_format format;
    mclw_uart_rx_t on_rx;
    void *on_rx_ctx;
    mclw_uart_break_t on_break;
    void *on_break_ctx;
    // Breaks the worker has yet to deliver, by the ring position they follow.
    uint32_t brk_at[UART_BREAKS_MAX];
    uint32_t brk_head;
    uint32_t brk_tail;
    // Received bytes the worker has yet to deliver.
    uint8_t ring[RX_RING];
    uint32_t head;
    uint32_t tail;
    // When the last byte sent toward the DUT finishes, in virtual ns.
    uint64_t tx_busy_until_ns;
    // A frame being decoded from the DUT's edges: when its start bit
    // fell, and each edge since.
    bool in_frame;
    uint64_t frame_start_ns;
    uint8_t edge_count;
    uint64_t edge_ns[FRAME_EDGES];
    uint8_t edge_level[FRAME_EDGES];
    // Whether the DUT's TX is watched, and the DUT's RX driven idle, for
    // a line that carries bits.
    bool watched;
    bool idled;
    struct mclw_uart_stats stats;
    struct mclw_work work;
};

static struct mclw_uart uarts[UARTS_MAX];

static uint32_t frame_bits(const struct mclw_uart *u) {
    return 1u + u->format.data_bits + u->format.stop_bits + (u->format.parity != MCLW_UART_PARITY_NONE);
}

static uint64_t frame_ns(const struct mclw_uart *u) {
    return u->baud != 0u ? (uint64_t)frame_bits(u) * 1000000000u / u->baud : 0u;
}

// Bit `bit` of a frame, 0 the start bit, in virtual ns from its start.
static uint64_t bit_at_ns(const struct mclw_uart *u, uint32_t bit) {
    return (uint64_t)bit * 1000000000u / u->baud;
}

// Called with the critical section held.
static void break_put(struct mclw_uart *u) {
    u->stats.breaks++;
    if (u->brk_head - u->brk_tail < UART_BREAKS_MAX) {
        u->brk_at[u->brk_head++ % UART_BREAKS_MAX] = u->head;
    }
}

static void ring_put(struct mclw_uart *u, uint8_t byte) {
    if (u->head - u->tail >= RX_RING) {
        u->stats.ring_overruns++;
        u->stats.overruns++;
    } else {
        u->ring[u->head++ % RX_RING] = byte;
        u->stats.bytes_rx++;
    }
}

// The line's level at `t_ns` within the frame being decoded.
static int frame_level(const struct mclw_uart *u, uint64_t t_ns) {
    int level = 0;

    for (uint8_t i = 0; i < u->edge_count && u->edge_ns[i] <= t_ns; i++) {
        level = u->edge_level[i];
    }
    return level;
}

// Ends the frame being decoded once the middle of its last stop bit has
// gone by: its data bits, LSB first, each read at its middle. Called with
// the critical section held; true when a byte went into the ring.
static bool frame_finish(struct mclw_uart *u, uint64_t now_ns) {
    uint32_t bits = frame_bits(u);
    uint64_t start = u->frame_start_ns;
    uint8_t byte = 0;

    if (!u->in_frame || now_ns < start + bit_at_ns(u, bits - 1u) + bit_at_ns(u, 1u) / 2u) {
        return false;
    }
    // A start bit is still low in its middle, as a UART checks it: a
    // shorter dip (a pin glitching as the DUT sets it up) is no frame.
    if (frame_level(u, start + bit_at_ns(u, 1u) / 2u) != 0) {
        u->in_frame = false;
        return false;
    }
    for (uint32_t i = 0; i < u->format.data_bits && i < 8u; i++) {
        uint64_t mid = start + bit_at_ns(u, 1u + i) + bit_at_ns(u, 1u) / 2u;

        byte |= (uint8_t)(frame_level(u, mid) << i);
    }
    u->in_frame = false;
    if (frame_level(u, start + bit_at_ns(u, bits - 1u) + bit_at_ns(u, 1u) / 2u) == 0) {
        // All zeros through the stop bit: a break, not a byte.
        if (byte == 0u) {
            break_put(u);
            return true;
        }
        u->stats.frame_errors++;
    }
    ring_put(u, byte);
    return true;
}

void mclw_host_uart_edge(uint8_t pin, int level, uint64_t t_ns) {
    bool got = false;
    struct mclw_uart *found = NULL;
    uint32_t state = mclw_hal_critical_enter();

    for (size_t i = 0; i < UARTS_MAX; i++) {
        struct mclw_uart *u = &uarts[i];

        if (u->owner == NULL || u->rx != pin || u->baud == 0u || mclw_host_pin_is_uart_tx(pin)) {
            continue;
        }
        found = u;
        got |= frame_finish(u, t_ns);
        if (!u->in_frame) {
            if (level == 0) {
                u->in_frame = true;
                u->frame_start_ns = t_ns;
                u->edge_count = 0;
            }
        } else if (u->edge_count < FRAME_EDGES) {
            u->edge_ns[u->edge_count] = t_ns;
            u->edge_level[u->edge_count] = level != 0;
            u->edge_count++;
        }
    }
    mclw_hal_critical_exit(state);
    if (got) {
        mclw_hal_work_post(&found->work);
    }
}

void mclw_host_uart_poll(uint64_t now_ns) {
    for (size_t i = 0; i < UARTS_MAX; i++) {
        uint32_t state = mclw_hal_critical_enter();
        bool got = uarts[i].owner != NULL && frame_finish(&uarts[i], now_ns);

        mclw_hal_critical_exit(state);
        if (got) {
            mclw_hal_work_post(&uarts[i].work);
        }
    }
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
        bool at_break = u->brk_tail != u->brk_head;
        uint32_t until = at_break ? u->brk_at[u->brk_tail % UART_BREAKS_MAX] : u->head;
        uint32_t n = until - u->tail;

        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = u->ring[(u->tail + i) % RX_RING];
        }
        u->tail += n;
        if (at_break) {
            u->brk_tail++;
        }
        mclw_hal_critical_exit(state);
        if (u->owner == NULL) {
            break;
        }
        if (n != 0u && u->on_rx != NULL) {
            u->on_rx(u, chunk, n, u->on_rx_ctx);
        }
        if (at_break && u->on_break != NULL) {
            u->on_break(u, u->on_break_ctx);
        }
        if (n == 0u && !at_break) {
            break;
        }
    }
    mclw_hal_unlock();
}

void mclw_host_uart_receive_break(uint8_t dut_tx_pin) {
    uint32_t state = mclw_hal_critical_enter();
    struct mclw_uart *found = NULL;

    for (size_t i = 0; i < UARTS_MAX && found == NULL; i++) {
        if (uarts[i].owner != NULL && uarts[i].rx == dut_tx_pin) {
            found = &uarts[i];
        }
    }
    if (found != NULL) {
        break_put(found);
    }
    mclw_hal_critical_exit(state);
    if (found != NULL) {
        mclw_hal_work_post(&found->work);
    }
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
            ring_put(found, bytes[i]);
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
    mclw_host_uart_route_changed();
    return u;
}

// A line that carries bits has the DUT's TX watched, and the DUT's RX
// driven high, idle; a bridged UART's bytes need neither. A watch stays
// (edges on a routed pin are passed over); the idle drive is released once
// the DUT routes its RX, which a UART attached before the DUT set its pins
// up would otherwise hold as a plain wire.
void mclw_host_uart_route_changed(void) {
    for (size_t i = 0; i < UARTS_MAX; i++) {
        struct mclw_uart *u = &uarts[i];

        if (u->owner == NULL) {
            continue;
        }
        if (!u->watched && !mclw_host_pin_is_uart_tx(u->rx)) {
            u->watched = true;
            mclw_host_event_watch(u->rx);
        }
        bool routed = mclw_host_pin_is_uart_rx(u->tx);

        if (!u->idled && !routed) {
            u->idled = true;
            mclw_host_event_gpio(u->tx, MCLW_HOST_GPIO_HIGH);
        } else if (u->idled && routed) {
            u->idled = false;
            mclw_host_event_gpio(u->tx, MCLW_HOST_GPIO_RELEASED);
        }
    }
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

// One frame as level actions on the DUT's RX pin from `at`: a start bit,
// the data LSB first, the parity, and the stop bits; only changes go out.
static void send_bits(const struct mclw_uart *u, uint8_t byte, uint64_t at) {
    uint32_t bits = frame_bits(u);
    uint32_t ones = 0;
    int last = 1;

    for (uint32_t bit = 0; bit < bits; bit++) {
        int level;

        if (bit == 0u) {
            level = 0;
        } else if (bit <= u->format.data_bits) {
            level = (byte >> (bit - 1u)) & 1;
            ones += (uint32_t)level;
        } else if (bit == u->format.data_bits + 1u && u->format.parity != MCLW_UART_PARITY_NONE) {
            level = (int)((ones & 1u) ^ (u->format.parity == MCLW_UART_PARITY_ODD));
        } else {
            level = 1;
        }
        if (level != last) {
            mclw_host_event_gpio_at(u->tx, level ? MCLW_HOST_GPIO_HIGH : MCLW_HOST_GPIO_LOW, at + bit_at_ns(u, bit));
            last = level;
        }
    }
}

size_t mclw_hal_uart_send(struct mclw_uart *u, const uint8_t *bytes, size_t len) {
    uint64_t frame = frame_ns(u);
    uint32_t state = mclw_hal_critical_enter();
    uint64_t at = mclw_host_event_ns();

    if (u->tx_busy_until_ns > at) {
        at = u->tx_busy_until_ns;
    }
    bool bytes_whole = mclw_host_pin_is_uart_rx(u->tx);

    for (size_t i = 0; i < len; i++) {
        if (bytes_whole) {
            mclw_host_event_uart(u->tx, bytes[i], at + frame);
        } else {
            send_bits(u, bytes[i], at);
        }
        at += frame;
    }
    u->tx_busy_until_ns = at;
    u->stats.bytes_tx += (uint32_t)len;
    mclw_hal_critical_exit(state);
    return len;
}

void mclw_hal_uart_set_break(struct mclw_uart *u, mclw_uart_break_t fn, void *ctx) {
    uint32_t state = mclw_hal_critical_enter();
    u->on_break = fn;
    u->on_break_ctx = ctx;
    mclw_hal_critical_exit(state);
}

// After what is queued: a `break` action to a DUT whose UART takes whole
// frames, or the line held low for `bits` bit times.
int mclw_hal_uart_send_break(struct mclw_uart *u, uint32_t bits) {
    if (u == NULL || u->owner == NULL || bits == 0u) {
        return -EINVAL;
    }
    uint32_t state = mclw_hal_critical_enter();
    uint64_t at = mclw_host_event_ns();

    if (u->tx_busy_until_ns > at) {
        at = u->tx_busy_until_ns;
    }
    uint64_t end = at + bit_at_ns(u, bits);
    if (mclw_host_pin_is_uart_rx(u->tx)) {
        mclw_host_event_uart_break(u->tx, end);
    } else {
        mclw_host_event_gpio_at(u->tx, MCLW_HOST_GPIO_LOW, at);
        mclw_host_event_gpio_at(u->tx, MCLW_HOST_GPIO_HIGH, end);
    }
    u->tx_busy_until_ns = end + bit_at_ns(u, 1u);
    mclw_hal_critical_exit(state);
    return 0;
}

void mclw_hal_uart_stats(const struct mclw_uart *u, struct mclw_uart_stats *out) {
    uint32_t state = mclw_hal_critical_enter();

    *out = u->stats;
    mclw_hal_critical_exit(state);
}
