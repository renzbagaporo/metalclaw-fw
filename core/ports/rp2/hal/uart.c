// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/uart.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

#include "metalclaw/bus.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/uart.h"

#include "uart_internal.h"

#define UART_BLOCK_COUNT 2u
#define UART_RX_RING_SIZE 512u
#define UART_TX_RING_SIZE 512u
#define UART_RX_RING_MASK (UART_RX_RING_SIZE - 1u)
#define UART_TX_RING_MASK (UART_TX_RING_SIZE - 1u)
#define UART_FIFO_LEVEL_EIGHTH 0u

struct pl011 {
    struct mclw_uart base;
    uart_inst_t *inst;
    uint32_t irq;
    uint8_t tx_pin;
    uint8_t rx_pin;
    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
    volatile uint16_t tx_head;
    volatile uint16_t tx_tail;
    uint8_t rx_ring[UART_RX_RING_SIZE];
    uint8_t tx_ring[UART_TX_RING_SIZE];
    uint8_t rx_linear[UART_RX_RING_SIZE];
};

static struct pl011 blocks[UART_BLOCK_COUNT];
static struct mclw_work rx_work[UART_BLOCK_COUNT];

// The UART function puts TX on GPIO 4n and RX on 4n+1; the RP2350's
// UART_AUX function adds TX on 4n+2 and RX on 4n+3. Either way the block
// alternates every eight pins starting at GPIO4: 0-3 UART0, 4-11 UART1,
// 12-19 UART0, and so on.
static bool uart_pin(uint8_t pin, uint8_t phase) {
#if PICO_RP2040
    return pin < NUM_BANK0_GPIOS && pin % 4u == phase;
#else
    return pin < NUM_BANK0_GPIOS && pin % 2u == phase;
#endif
}

static uart_inst_t *instance_for(uint8_t tx, uint8_t rx) {
    uint8_t block = (uint8_t)(((tx + 4u) / 8u) % 2u);

    if (!uart_pin(tx, 0u) || !uart_pin(rx, 1u) || block != ((rx + 4u) / 8u) % 2u) {
        return NULL;
    }
    return UART_INSTANCE(block);
}

static bool parity_for(enum mclw_uart_parity parity, uart_parity_t *out) {
    switch (parity) {
        case MCLW_UART_PARITY_NONE:
            *out = UART_PARITY_NONE;
            return true;
        case MCLW_UART_PARITY_EVEN:
            *out = UART_PARITY_EVEN;
            return true;
        case MCLW_UART_PARITY_ODD:
            *out = UART_PARITY_ODD;
            return true;
    }
    return false;
}

static bool format_valid(const struct mclw_uart_format *format) {
    uart_parity_t parity;

    return format != NULL && format->data_bits >= 5 && format->data_bits <= 8 &&
        format->stop_bits >= 1 && format->stop_bits <= 2 && parity_for(format->parity, &parity);
}

static void rx_deliver(struct mclw_work *work) {
    struct pl011 *u = work->ctx;
    uint16_t head = u->rx_head;
    uint16_t tail = u->rx_tail;
    uint16_t used = (uint16_t)((head - tail) & UART_RX_RING_MASK);

    if (used == 0u) {
        return;
    }
    if (!mclw_hal_try_lock()) {
        mclw_hal_work_post(work);
        return;
    }
    if (u->base.owner == NULL || u->base.on_rx == NULL) {
        u->rx_tail = head;
        mclw_hal_unlock();
        return;
    }
    for (uint16_t i = 0; i < used; i++) {
        u->rx_linear[i] = u->rx_ring[(tail + i) & UART_RX_RING_MASK];
    }
    __compiler_memory_barrier();
    u->rx_tail = (uint16_t)((tail + used) & UART_RX_RING_MASK);
    u->base.on_rx(&u->base, u->rx_linear, used, u->base.on_rx_ctx);
    mclw_hal_unlock();
}

static void rx_service(struct pl011 *u) {
    uart_hw_t *hw = uart_get_hw(u->inst);
    uint16_t head = u->rx_head;
    bool pushed = false;

    hw->icr = UART_UARTICR_RXIC_BITS | UART_UARTICR_RTIC_BITS;
    while ((hw->fr & UART_UARTFR_RXFE_BITS) == 0u) {
        uint32_t dr = hw->dr;
        uint16_t next = (uint16_t)((head + 1u) & UART_RX_RING_MASK);

        if ((dr & UART_UARTDR_OE_BITS) != 0u) {
            u->base.stats.overruns++;
            hw->rsr = 0u;
        }
        if ((dr & (UART_UARTDR_FE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_BE_BITS)) != 0u) {
            u->base.stats.frame_errors++;
            continue;
        }
        if (next == u->rx_tail) {
            u->base.stats.overruns++;
            continue;
        }
        u->rx_ring[head] = (uint8_t)dr;
        head = next;
        pushed = true;
        u->base.stats.bytes_rx++;
    }
    if (pushed) {
        __compiler_memory_barrier();
        u->rx_head = head;
        mclw_hal_work_post(&rx_work[uart_get_index(u->inst)]);
    }
}

static void tx_pump(struct pl011 *u) {
    uart_hw_t *hw = uart_get_hw(u->inst);
    uint16_t tail = u->tx_tail;

    while (tail != u->tx_head && (hw->fr & UART_UARTFR_TXFF_BITS) == 0u) {
        hw->dr = u->tx_ring[tail];
        tail = (uint16_t)((tail + 1u) & UART_TX_RING_MASK);
        u->base.stats.bytes_tx++;
    }
    u->tx_tail = tail;
    if (tail == u->tx_head) {
        hw_clear_bits(&hw->imsc, UART_UARTIMSC_TXIM_BITS);
    } else {
        hw_set_bits(&hw->imsc, UART_UARTIMSC_TXIM_BITS);
    }
}

static void service(struct pl011 *u) {
    uart_hw_t *hw = uart_get_hw(u->inst);
    uint32_t state = mclw_hal_critical_enter();
    uint32_t mis = hw->mis;

    if ((mis & (UART_UARTMIS_RXMIS_BITS | UART_UARTMIS_RTMIS_BITS)) != 0u) {
        rx_service(u);
    }
    if ((mis & UART_UARTMIS_TXMIS_BITS) != 0u) {
        hw->icr = UART_UARTICR_TXIC_BITS;
        tx_pump(u);
    }
    mclw_hal_critical_exit(state);
}

static void uart0_irq_handler(void) {
    service(&blocks[0]);
}

static void uart1_irq_handler(void) {
    service(&blocks[1]);
}

static irq_handler_t handler_for(const uart_inst_t *inst) {
    return inst == uart0 ? uart0_irq_handler : uart1_irq_handler;
}

static void irq_enable_on_worker(void *ctx) {
    const struct pl011 *u = ctx;

    irq_set_enabled(u->irq, true);
}

static void irq_disable_on_worker(void *ctx) {
    const struct pl011 *u = ctx;

    irq_set_enabled(u->irq, false);
}

static void tx_flush(struct pl011 *u) {
    for (;;) {
        uint32_t state = mclw_hal_critical_enter();
        bool empty = u->tx_head == u->tx_tail;
        if (!empty) {
            tx_pump(u);
        }
        mclw_hal_critical_exit(state);
        if (empty) {
            break;
        }
    }
    uart_tx_wait_blocking(u->inst);
}

static void pl011_close(struct mclw_uart *uart) {
    struct pl011 *u = (struct pl011 *)uart;
    uart_hw_t *hw = uart_get_hw(u->inst);

    hw->imsc = 0u;
    hw->icr = UART_UARTICR_BITS;
    mclw_hal_work_call(irq_disable_on_worker, u);
    irq_remove_handler(u->irq, handler_for(u->inst));
    uart_deinit(u->inst);
    gpio_set_pulls(u->rx_pin, false, false);
    gpio_deinit(u->tx_pin);
    gpio_deinit(u->rx_pin);
    mclw_hal_pin_release(u->tx_pin, u->base.owner);
    mclw_hal_pin_release(u->rx_pin, u->base.owner);
    u->base.owner = NULL;
    u->base.on_rx = NULL;
    u->base.on_rx_ctx = NULL;
    u->rx_head = 0;
    u->rx_tail = 0;
    u->tx_head = 0;
    u->tx_tail = 0;
}

static int pl011_set_baud(struct mclw_uart *uart, uint32_t baud) {
    struct pl011 *u = (struct pl011 *)uart;

    tx_flush(u);

    uart_hw_t *hw = uart_get_hw(u->inst);
    uint32_t imsc = hw->imsc;
    hw->imsc = 0u;
    u->base.baud = uart_set_baudrate(u->inst, baud);
    hw->icr = UART_UARTICR_BITS;
    hw->rsr = 0u;
    hw->imsc = imsc;
    return u->base.baud == 0u ? -EIO : 0;
}

static int pl011_set_format(struct mclw_uart *uart, const struct mclw_uart_format *format) {
    struct pl011 *u = (struct pl011 *)uart;
    uart_parity_t parity = UART_PARITY_NONE;

    (void)parity_for(format->parity, &parity);
    tx_flush(u);

    uart_hw_t *hw = uart_get_hw(u->inst);
    uint32_t imsc = hw->imsc;
    hw->imsc = 0u;
    uart_set_format(u->inst, format->data_bits, format->stop_bits, parity);
    hw->icr = UART_UARTICR_BITS;
    hw->rsr = 0u;
    hw->imsc = imsc;
    return 0;
}

static size_t pl011_send(struct mclw_uart *uart, const uint8_t *bytes, size_t len) {
    struct pl011 *u = (struct pl011 *)uart;
    uint16_t head = u->tx_head;
    size_t free_slots = (size_t)(UART_TX_RING_MASK - ((head - u->tx_tail) & UART_TX_RING_MASK));
    size_t queued = len < free_slots ? len : free_slots;

    if (queued == 0) {
        return 0;
    }
    for (size_t i = 0; i < queued; i++) {
        u->tx_ring[(head + i) & UART_TX_RING_MASK] = bytes[i];
    }
    __compiler_memory_barrier();
    u->tx_head = (uint16_t)((head + queued) & UART_TX_RING_MASK);

    uint32_t state = mclw_hal_critical_enter();
    tx_pump(u);
    mclw_hal_critical_exit(state);
    return queued;
}

static const struct mclw_uart_engine pl011_engine = {
    .close = pl011_close,
    .set_baud = pl011_set_baud,
    .set_format = pl011_set_format,
    .send = pl011_send,
};

static struct mclw_uart *pl011_open(uart_inst_t *inst, uint8_t tx, uint8_t rx, uint32_t baud,
    const struct mclw_uart_format *format, const void *owner, int *err) {
    uart_parity_t parity = UART_PARITY_NONE;
    struct pl011 *u = &blocks[uart_get_index(inst)];
    uint32_t irq = inst == uart0 ? (uint32_t)UART0_IRQ : (uint32_t)UART1_IRQ;
    irq_handler_t handler = handler_for(inst);
    irq_handler_t previous = irq_get_exclusive_handler(irq);

    (void)parity_for(format->parity, &parity);
    if (previous != NULL && previous != handler) {
        if (irq_is_enabled(irq)) {
            *err = -EBUSY;
            return NULL;
        }
        irq_remove_handler(irq, previous);
    }
    *err = mclw_hal_pin_claim(tx, owner);
    if (*err == 0) {
        *err = mclw_hal_pin_claim(rx, owner);
        if (*err != 0) {
            mclw_hal_pin_release(tx, owner);
        }
    }
    if (*err != 0) {
        return NULL;
    }
    u->base.engine = &pl011_engine;
    u->inst = inst;
    u->irq = irq;
    u->tx_pin = tx;
    u->rx_pin = rx;
    u->base.on_rx = NULL;
    u->base.on_rx_ctx = NULL;
    u->base.stats.bytes_rx = 0;
    u->base.stats.bytes_tx = 0;
    u->base.stats.overruns = 0;
    u->base.stats.frame_errors = 0;
    u->rx_head = 0;
    u->rx_tail = 0;
    u->tx_head = 0;
    u->tx_tail = 0;
    rx_work[uart_get_index(inst)].fn = rx_deliver;
    rx_work[uart_get_index(inst)].ctx = u;

    // The pins are connected before the block is enabled: the SDK's format
    // and FIFO writes each cycle the UART with character-time waits, and a
    // receiver running on an unconnected input reads it low and logs a
    // break the moment the pin comes up.
    gpio_set_function(tx, UART_FUNCSEL_NUM(inst, tx));
    gpio_set_function(rx, UART_FUNCSEL_NUM(inst, rx));
    gpio_pull_up(rx);
    u->base.baud = uart_init(inst, baud);
    if (u->base.baud == 0u) {
        uart_deinit(inst);
        mclw_hal_pin_release(tx, owner);
        mclw_hal_pin_release(rx, owner);
        *err = -EIO;
        return NULL;
    }
    uart_set_format(inst, format->data_bits, format->stop_bits, parity);
    __dsb();
    uart_set_hw_flow(inst, false, false);
    uart_set_fifo_enabled(inst, true);
    __dsb();

    uart_hw_t *hw = uart_get_hw(inst);
    hw_write_masked(&hw->ifls,
        (UART_FIFO_LEVEL_EIGHTH << UART_UARTIFLS_RXIFLSEL_LSB) |
        (UART_FIFO_LEVEL_EIGHTH << UART_UARTIFLS_TXIFLSEL_LSB),
        UART_UARTIFLS_RXIFLSEL_BITS | UART_UARTIFLS_TXIFLSEL_BITS);
    hw->icr = UART_UARTICR_BITS;
    hw->rsr = 0u;
    u->base.owner = owner;

    irq_set_exclusive_handler(irq, handler);
    mclw_hal_work_call(irq_enable_on_worker, u);
    hw->imsc = UART_UARTIMSC_RXIM_BITS | UART_UARTIMSC_RTIM_BITS;
    *err = 0;
    return &u->base;
}

// A pin pair that carries one PL011's TX and RX functions gets that block
// while it is free; every other pair, and a pair whose block is taken, gets
// the PIO engine.
struct mclw_uart *mclw_hal_uart_open(uint8_t tx, uint8_t rx, uint32_t baud,
    const struct mclw_uart_format *format, const void *owner, int *err) {
    if (owner == NULL || baud == 0u || !format_valid(format) || tx == rx) {
        *err = -EINVAL;
        return NULL;
    }
    uart_inst_t *inst = instance_for(tx, rx);
    if (inst != NULL && blocks[uart_get_index(inst)].base.owner == NULL) {
        return pl011_open(inst, tx, rx, baud, format, owner, err);
    }
    return mclw_uart_pio_open(tx, rx, baud, format, owner, err);
}

void mclw_hal_uart_close(struct mclw_uart *u) {
    if (u == NULL || u->owner == NULL) {
        return;
    }
    u->engine->close(u);
}

int mclw_hal_uart_set_baud(struct mclw_uart *u, uint32_t baud) {
    if (u == NULL || u->owner == NULL || baud == 0u) {
        return -EINVAL;
    }
    return u->engine->set_baud(u, baud);
}

uint32_t mclw_hal_uart_baud(const struct mclw_uart *u) {
    return u == NULL ? 0u : u->baud;
}

int mclw_hal_uart_set_format(struct mclw_uart *u, const struct mclw_uart_format *format) {
    if (u == NULL || u->owner == NULL || !format_valid(format)) {
        return -EINVAL;
    }
    return u->engine->set_format(u, format);
}

void mclw_hal_uart_set_rx(struct mclw_uart *u, mclw_uart_rx_t fn, void *ctx) {
    if (u == NULL) {
        return;
    }
    uint32_t state = mclw_hal_critical_enter();
    u->on_rx = fn;
    u->on_rx_ctx = ctx;
    mclw_hal_critical_exit(state);
}

size_t mclw_hal_uart_send(struct mclw_uart *u, const uint8_t *bytes, size_t len) {
    if (u == NULL || u->owner == NULL || bytes == NULL || len == 0) {
        return 0;
    }
    return u->engine->send(u, bytes, len);
}

void mclw_hal_uart_stats(const struct mclw_uart *u, struct mclw_uart_stats *out) {
    if (u == NULL || out == NULL) {
        return;
    }
    uint32_t state = mclw_hal_critical_enter();
    *out = u->stats;
    mclw_hal_critical_exit(state);
}
