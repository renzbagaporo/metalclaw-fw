// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "py/runtime.h"

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

#define UART_BLOCK_COUNT 2u
#define UART_RX_RING_SIZE 512u
#define UART_TX_RING_SIZE 512u
#define UART_RX_RING_MASK (UART_RX_RING_SIZE - 1u)
#define UART_TX_RING_MASK (UART_TX_RING_SIZE - 1u)
#define UART_FIFO_LEVEL_EIGHTH 0u

struct metalclaw_uart {
    uart_inst_t *inst;
    uint32_t irq;
    uint8_t tx_pin;
    uint8_t rx_pin;
    const void *owner;
    uint32_t baud;
    metalclaw_uart_rx_t on_rx;
    void *on_rx_ctx;
    struct metalclaw_uart_stats stats;
    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
    volatile uint16_t tx_head;
    volatile uint16_t tx_tail;
    uint8_t rx_ring[UART_RX_RING_SIZE];
    uint8_t tx_ring[UART_TX_RING_SIZE];
    uint8_t rx_linear[UART_RX_RING_SIZE];
};

static struct metalclaw_uart blocks[UART_BLOCK_COUNT];
static mp_sched_node_t rx_nodes[UART_BLOCK_COUNT];

static uart_inst_t *instance_for(uint8_t tx, uint8_t rx) {
    bool uart0_tx = tx == 0u || tx == 12u || tx == 16u;
    bool uart0_rx = rx == 1u || rx == 13u || rx == 17u;
    bool uart1_tx = tx == 4u || tx == 8u || tx == 20u;
    bool uart1_rx = rx == 5u || rx == 9u || rx == 21u;

    if (uart0_tx && uart0_rx) {
        return uart0;
    }
    if (uart1_tx && uart1_rx) {
        return uart1;
    }
    return NULL;
}

static bool parity_for(enum metalclaw_uart_parity parity, uart_parity_t *out) {
    switch (parity) {
        case METALCLAW_UART_PARITY_NONE:
            *out = UART_PARITY_NONE;
            return true;
        case METALCLAW_UART_PARITY_EVEN:
            *out = UART_PARITY_EVEN;
            return true;
        case METALCLAW_UART_PARITY_ODD:
            *out = UART_PARITY_ODD;
            return true;
    }
    return false;
}

static void rx_deliver(mp_sched_node_t *node) {
    size_t index = (size_t)(node - rx_nodes);
    if (index >= UART_BLOCK_COUNT) {
        return;
    }
    struct metalclaw_uart *u = &blocks[index];
    uint16_t head = u->rx_head;
    uint16_t tail = u->rx_tail;
    uint16_t used = (uint16_t)((head - tail) & UART_RX_RING_MASK);

    if (used == 0u) {
        return;
    }
    if (u->owner == NULL || u->on_rx == NULL) {
        u->rx_tail = head;
        return;
    }
    for (uint16_t i = 0; i < used; i++) {
        u->rx_linear[i] = u->rx_ring[(tail + i) & UART_RX_RING_MASK];
    }
    __compiler_memory_barrier();
    u->rx_tail = (uint16_t)((tail + used) & UART_RX_RING_MASK);
    u->on_rx(u, u->rx_linear, used, u->on_rx_ctx);
}

static void rx_service(struct metalclaw_uart *u) {
    uart_hw_t *hw = uart_get_hw(u->inst);
    uint16_t head = u->rx_head;
    bool pushed = false;

    hw->icr = UART_UARTICR_RXIC_BITS | UART_UARTICR_RTIC_BITS;
    while ((hw->fr & UART_UARTFR_RXFE_BITS) == 0u) {
        uint32_t dr = hw->dr;
        uint16_t next = (uint16_t)((head + 1u) & UART_RX_RING_MASK);

        if ((dr & UART_UARTDR_OE_BITS) != 0u) {
            u->stats.overruns++;
            hw->rsr = 0u;
        }
        if ((dr & (UART_UARTDR_FE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_BE_BITS)) != 0u) {
            u->stats.frame_errors++;
            continue;
        }
        if (next == u->rx_tail) {
            u->stats.overruns++;
            continue;
        }
        u->rx_ring[head] = (uint8_t)dr;
        head = next;
        pushed = true;
        u->stats.bytes_rx++;
    }
    if (pushed) {
        __compiler_memory_barrier();
        u->rx_head = head;
        (void)mp_sched_schedule_node(&rx_nodes[uart_get_index(u->inst)], rx_deliver);
    }
}

static void tx_pump(struct metalclaw_uart *u) {
    uart_hw_t *hw = uart_get_hw(u->inst);
    uint16_t tail = u->tx_tail;

    while (tail != u->tx_head && (hw->fr & UART_UARTFR_TXFF_BITS) == 0u) {
        hw->dr = u->tx_ring[tail];
        tail = (uint16_t)((tail + 1u) & UART_TX_RING_MASK);
        u->stats.bytes_tx++;
    }
    u->tx_tail = tail;
    if (tail == u->tx_head) {
        hw_clear_bits(&hw->imsc, UART_UARTIMSC_TXIM_BITS);
    } else {
        hw_set_bits(&hw->imsc, UART_UARTIMSC_TXIM_BITS);
    }
}

static void service(struct metalclaw_uart *u) {
    uart_hw_t *hw = uart_get_hw(u->inst);
    uint32_t mis = hw->mis;

    if ((mis & (UART_UARTMIS_RXMIS_BITS | UART_UARTMIS_RTMIS_BITS)) != 0u) {
        rx_service(u);
    }
    if ((mis & UART_UARTMIS_TXMIS_BITS) != 0u) {
        hw->icr = UART_UARTICR_TXIC_BITS;
        tx_pump(u);
    }
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

static void tx_flush(struct metalclaw_uart *u) {
    for (;;) {
        uint32_t state = metalclaw_hal_critical_enter();
        bool empty = u->tx_head == u->tx_tail;
        if (!empty) {
            tx_pump(u);
        }
        metalclaw_hal_critical_exit(state);
        if (empty) {
            break;
        }
    }
    uart_tx_wait_blocking(u->inst);
}

struct metalclaw_uart *metalclaw_hal_uart_open(uint8_t tx, uint8_t rx, uint32_t baud,
    const struct metalclaw_uart_format *format, const void *owner, int *err) {
    uart_inst_t *inst = instance_for(tx, rx);
    uart_parity_t parity = UART_PARITY_NONE;

    if (inst == NULL || owner == NULL || format == NULL || baud == 0u ||
        format->data_bits < 5 || format->data_bits > 8 ||
        format->stop_bits < 1 || format->stop_bits > 2 ||
        !parity_for(format->parity, &parity)) {
        *err = -EINVAL;
        return NULL;
    }
    struct metalclaw_uart *u = &blocks[uart_get_index(inst)];
    if (u->owner != NULL) {
        *err = -EBUSY;
        return NULL;
    }
    uint32_t irq = inst == uart0 ? (uint32_t)UART0_IRQ : (uint32_t)UART1_IRQ;
    irq_handler_t handler = handler_for(inst);
    irq_handler_t previous = irq_get_exclusive_handler(irq);
    if (previous != NULL && previous != handler) {
        if (irq_is_enabled(irq)) {
            *err = -EBUSY;
            return NULL;
        }
        irq_remove_handler(irq, previous);
    }
    *err = metalclaw_hal_pin_claim(tx, owner);
    if (*err == 0) {
        *err = metalclaw_hal_pin_claim(rx, owner);
        if (*err != 0) {
            metalclaw_hal_pin_release(tx, owner);
        }
    }
    if (*err != 0) {
        return NULL;
    }
    u->inst = inst;
    u->irq = irq;
    u->tx_pin = tx;
    u->rx_pin = rx;
    u->on_rx = NULL;
    u->on_rx_ctx = NULL;
    u->stats.bytes_rx = 0;
    u->stats.bytes_tx = 0;
    u->stats.overruns = 0;
    u->stats.frame_errors = 0;
    u->rx_head = 0;
    u->rx_tail = 0;
    u->tx_head = 0;
    u->tx_tail = 0;

    u->baud = uart_init(inst, baud);
    if (u->baud == 0u) {
        uart_deinit(inst);
        metalclaw_hal_pin_release(tx, owner);
        metalclaw_hal_pin_release(rx, owner);
        *err = -EIO;
        return NULL;
    }
    uart_set_format(inst, format->data_bits, format->stop_bits, parity);
    __dsb();
    uart_set_hw_flow(inst, false, false);
    uart_set_fifo_enabled(inst, true);
    __dsb();
    gpio_set_function(tx, UART_FUNCSEL_NUM(inst, tx));
    gpio_set_function(rx, UART_FUNCSEL_NUM(inst, rx));
    gpio_pull_up(rx);

    uart_hw_t *hw = uart_get_hw(inst);
    hw_write_masked(&hw->ifls,
        (UART_FIFO_LEVEL_EIGHTH << UART_UARTIFLS_RXIFLSEL_LSB) |
        (UART_FIFO_LEVEL_EIGHTH << UART_UARTIFLS_TXIFLSEL_LSB),
        UART_UARTIFLS_RXIFLSEL_BITS | UART_UARTIFLS_TXIFLSEL_BITS);
    hw->icr = UART_UARTICR_BITS;
    hw->rsr = 0u;
    u->owner = owner;

    irq_set_exclusive_handler(irq, handler);
    irq_set_enabled(irq, true);
    hw->imsc = UART_UARTIMSC_RXIM_BITS | UART_UARTIMSC_RTIM_BITS;
    *err = 0;
    return u;
}

void metalclaw_hal_uart_close(struct metalclaw_uart *u) {
    if (u == NULL || u->owner == NULL) {
        return;
    }
    uart_hw_t *hw = uart_get_hw(u->inst);
    hw->imsc = 0u;
    hw->icr = UART_UARTICR_BITS;
    irq_set_enabled(u->irq, false);
    irq_remove_handler(u->irq, handler_for(u->inst));
    uart_deinit(u->inst);
    gpio_set_pulls(u->rx_pin, false, false);
    gpio_deinit(u->tx_pin);
    gpio_deinit(u->rx_pin);
    metalclaw_hal_pin_release(u->tx_pin, u->owner);
    metalclaw_hal_pin_release(u->rx_pin, u->owner);
    u->owner = NULL;
    u->on_rx = NULL;
    u->on_rx_ctx = NULL;
    u->rx_head = 0;
    u->rx_tail = 0;
    u->tx_head = 0;
    u->tx_tail = 0;
}

int metalclaw_hal_uart_set_baud(struct metalclaw_uart *u, uint32_t baud) {
    if (u == NULL || u->owner == NULL || baud == 0u) {
        return -EINVAL;
    }
    tx_flush(u);

    uart_hw_t *hw = uart_get_hw(u->inst);
    uint32_t imsc = hw->imsc;
    hw->imsc = 0u;
    u->baud = uart_set_baudrate(u->inst, baud);
    hw->icr = UART_UARTICR_BITS;
    hw->rsr = 0u;
    hw->imsc = imsc;
    return u->baud == 0u ? -EIO : 0;
}

uint32_t metalclaw_hal_uart_baud(const struct metalclaw_uart *u) {
    return u == NULL ? 0u : u->baud;
}

void metalclaw_hal_uart_set_rx(struct metalclaw_uart *u, metalclaw_uart_rx_t fn, void *ctx) {
    if (u == NULL) {
        return;
    }
    uint32_t state = metalclaw_hal_critical_enter();
    u->on_rx = fn;
    u->on_rx_ctx = ctx;
    metalclaw_hal_critical_exit(state);
}

size_t metalclaw_hal_uart_send(struct metalclaw_uart *u, const uint8_t *bytes, size_t len) {
    if (u == NULL || u->owner == NULL || bytes == NULL || len == 0) {
        return 0;
    }
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

    uint32_t state = metalclaw_hal_critical_enter();
    tx_pump(u);
    metalclaw_hal_critical_exit(state);
    return queued;
}

void metalclaw_hal_uart_stats(const struct metalclaw_uart *u, struct metalclaw_uart_stats *out) {
    if (u == NULL || out == NULL) {
        return;
    }
    uint32_t state = metalclaw_hal_critical_enter();
    *out = u->stats;
    metalclaw_hal_critical_exit(state);
}
