// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
// The PIO programs follow uart_tx.pio and uart_rx.pio from pico-examples
// (BSD-3-Clause).
//
// hal/uart.h on any pin pair: one state machine transmits and one receives,
// eight PIO cycles per bit, 8 data bits, no parity, one or two stop bits. DMA
// moves every byte in both directions, so nothing here runs per byte; the
// worker's poll delivers what arrived and starts the next transmit run. A
// flash write that parks the worker costs nothing until the receive ring
// fills.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"

#include "metalclaw/bus.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/uart.h"

#include "pio_window.h"
#include "uart_internal.h"

#define UART_PIO_COUNT 2u
#define UART_PIO_CYCLES_PER_BIT 8u
#define UART_PIO_RX_RING_BITS 10u
#define UART_PIO_TX_RING_BITS 9u
#define UART_PIO_RX_RING_SIZE (1u << UART_PIO_RX_RING_BITS)
#define UART_PIO_TX_RING_SIZE (1u << UART_PIO_TX_RING_BITS)
#define UART_PIO_RX_RING_MASK (UART_PIO_RX_RING_SIZE - 1u)
#define UART_PIO_TX_RING_MASK (UART_PIO_TX_RING_SIZE - 1u)
// The receive channel is re-armed by the poll once it has run this many
// bytes: four billion, days of traffic even at 921600 baud.
#define UART_PIO_RX_DMA_COUNT 0xffffffffu
// A backlog this close to the ring size may already be overwritten while it
// is copied out, so it counts as an overrun.
#define UART_PIO_RX_SLACK 16u
// The receiver raises PIO flag 4 + its state machine number on a stop bit
// that is low: a framing error or a break.
#define UART_PIO_FRAME_FLAG 4u

enum {
    TX_STOP,
    TX_START,
    TX_BIT,
    TX_LOOP,
    TX_STOP_2,
    TX_LENGTH,
};

enum {
    RX_WAIT_START,
    RX_START,
    RX_BIT,
    RX_LOOP,
    RX_CHECK_STOP,
    RX_FRAME_ERROR,
    RX_WAIT_IDLE,
    RX_RESTART,
    RX_PUSH,
    RX_LENGTH,
};

struct uart_pio {
    struct mclw_uart base;
    bool active;
    PIO pio;
    uint8_t pio_index;
    uint8_t tx_sm;
    uint8_t rx_sm;
    uint8_t tx_pin;
    uint8_t rx_pin;
    uint8_t stop_bits;
    uint8_t tx_dma;
    uint8_t rx_dma;
    uint8_t *tx_ring;
    uint8_t *rx_ring;
    // Free-running transmit indices; the DMA reads [tail, tail + inflight).
    volatile uint32_t tx_head;
    volatile uint32_t tx_tail;
    uint32_t tx_inflight;
    // Receive bookkeeping, touched only by the worker's poll once active:
    // the ring offset the channel was armed at, the bytes taken since, and
    // the ring offset of the next byte to take.
    uint32_t rx_base;
    uint32_t rx_taken;
    uint32_t rx_read;
    uint8_t rx_linear[UART_PIO_RX_RING_SIZE];
};

struct pio_programs {
    uint8_t users;
    uint8_t tx_offset;
    uint8_t rx_offset;
};

static struct uart_pio slots[UART_PIO_COUNT];
static struct pio_programs programs[NUM_PIOS];
static uint8_t tx_rings[UART_PIO_COUNT][UART_PIO_TX_RING_SIZE] __attribute__((aligned(UART_PIO_TX_RING_SIZE)));
static uint8_t rx_rings[UART_PIO_COUNT][UART_PIO_RX_RING_SIZE] __attribute__((aligned(UART_PIO_RX_RING_SIZE)));
static uint16_t tx_instructions[TX_LENGTH];
static uint16_t rx_instructions[RX_LENGTH];
static bool poll_registered;

static const pio_program_t tx_program = {
    .instructions = tx_instructions,
    .length = TX_LENGTH,
    .origin = -1,
    .pio_version = 0,
};

static const pio_program_t rx_program = {
    .instructions = rx_instructions,
    .length = RX_LENGTH,
    .origin = -1,
    .pio_version = 0,
};

// The transmitter: the line rests high in the stop slot while the FIFO is
// empty. TX_STOP_2 is inside the wrap only for two stop bits. Side-set is one
// optional bit, which leaves three bits of delay.
static void encode_programs(void) {
    tx_instructions[TX_STOP] = (uint16_t)(pio_encode_pull(false, true) | pio_encode_sideset_opt(1, 1) |
        pio_encode_delay(7));
    tx_instructions[TX_START] = (uint16_t)(pio_encode_set(pio_x, 7) | pio_encode_sideset_opt(1, 0) |
        pio_encode_delay(7));
    tx_instructions[TX_BIT] = (uint16_t)pio_encode_out(pio_pins, 1);
    tx_instructions[TX_LOOP] = (uint16_t)(pio_encode_jmp_x_dec(TX_BIT) | pio_encode_delay(6));
    tx_instructions[TX_STOP_2] = (uint16_t)(pio_encode_nop() | pio_encode_sideset_opt(1, 1) |
        pio_encode_delay(7));

    // The receiver samples mid-bit: the start edge, then 12 cycles to the
    // middle of bit 0, then every 8. A low stop bit sets the frame flag and
    // drops the byte once the line is idle again.
    rx_instructions[RX_WAIT_START] = (uint16_t)pio_encode_wait_pin(false, 0);
    rx_instructions[RX_START] = (uint16_t)(pio_encode_set(pio_x, 7) | pio_encode_delay(10));
    rx_instructions[RX_BIT] = (uint16_t)pio_encode_in(pio_pins, 1);
    rx_instructions[RX_LOOP] = (uint16_t)(pio_encode_jmp_x_dec(RX_BIT) | pio_encode_delay(6));
    rx_instructions[RX_CHECK_STOP] = (uint16_t)pio_encode_jmp_pin(RX_PUSH);
    rx_instructions[RX_FRAME_ERROR] = (uint16_t)pio_encode_irq_set(true, UART_PIO_FRAME_FLAG);
    rx_instructions[RX_WAIT_IDLE] = (uint16_t)pio_encode_wait_pin(true, 0);
    rx_instructions[RX_RESTART] = (uint16_t)pio_encode_jmp(RX_WAIT_START);
    rx_instructions[RX_PUSH] = (uint16_t)pio_encode_push(false, true);
}

static bool divider_for(uint32_t baud, uint32_t *div256, uint32_t *actual) {
    uint64_t clock = clock_get_hz(clk_sys);
    uint64_t denominator = (uint64_t)UART_PIO_CYCLES_PER_BIT * baud;
    uint64_t d = (clock * 256u + denominator / 2u) / denominator;

    if (baud == 0u || d < 256u || d > 0xffffffu) {
        return false;
    }
    *div256 = (uint32_t)d;
    *actual = (uint32_t)((clock * 256u) / ((uint64_t)UART_PIO_CYCLES_PER_BIT * d));
    return true;
}

static bool format_supported(const struct mclw_uart_format *format) {
    return format->data_bits == 8u && format->parity == MCLW_UART_PARITY_NONE &&
        (format->stop_bits == 1u || format->stop_bits == 2u);
}

static uint32_t tx_wrap(const struct uart_pio *u) {
    return programs[u->pio_index].tx_offset + (u->stop_bits == 2u ? TX_STOP_2 : TX_LOOP);
}

static void apply_divider(struct uart_pio *u, uint32_t div256) {
    pio_sm_set_clkdiv_int_frac8(u->pio, u->tx_sm, div256 >> 8, (uint8_t)(div256 & 0xffu));
    pio_sm_set_clkdiv_int_frac8(u->pio, u->rx_sm, div256 >> 8, (uint8_t)(div256 & 0xffu));
    pio_clkdiv_restart_sm_mask(u->pio, (1u << u->tx_sm) | (1u << u->rx_sm));
}

// Under the critical section: retires a finished run and starts the next.
static void tx_service(struct uart_pio *u) {
    if (u->tx_inflight != 0u) {
        if (dma_channel_is_busy(u->tx_dma)) {
            return;
        }
        u->tx_tail += u->tx_inflight;
        u->base.stats.bytes_tx += u->tx_inflight;
        u->tx_inflight = 0;
    }
    uint32_t used = u->tx_head - u->tx_tail;
    if (used == 0u) {
        return;
    }
    u->tx_inflight = used;
    dma_channel_set_read_addr(u->tx_dma, &u->tx_ring[u->tx_tail & UART_PIO_TX_RING_MASK], false);
    dma_channel_set_trans_count(u->tx_dma, used, true);
}

static uint32_t frame_us(const struct uart_pio *u) {
    uint32_t bits = 1u + 8u + u->stop_bits;

    return (bits * 1000000u + u->base.baud - 1u) / u->base.baud;
}

// Everything queued is on the wire: the ring and the FIFO are empty and the
// frame the state machine pulled last has had time to finish.
static int tx_drain(struct uart_pio *u) {
    uint32_t frame = frame_us(u);
    uint64_t deadline = mclw_hal_time_us() +
        (uint64_t)(UART_PIO_TX_RING_SIZE + 8u + 2u) * frame + 1000u;

    for (;;) {
        uint32_t state = mclw_hal_critical_enter();
        tx_service(u);
        bool idle = u->tx_inflight == 0u && u->tx_head == u->tx_tail;
        mclw_hal_critical_exit(state);
        if (idle && pio_sm_is_tx_fifo_empty(u->pio, u->tx_sm)) {
            break;
        }
        if (mclw_hal_time_us() > deadline) {
            return -ETIMEDOUT;
        }
    }
    mclw_hal_delay_us(frame);
    return 0;
}

static void rx_deliver(struct uart_pio *u) {
    dma_channel_hw_t *ch = dma_channel_hw_addr(u->rx_dma);
    uint32_t remaining = ch->transfer_count;
    uint32_t pos = (ch->write_addr - (uint32_t)(uintptr_t)u->rx_ring) & UART_PIO_RX_RING_MASK;
    uint32_t issued = UART_PIO_RX_DMA_COUNT - remaining;

    if (issued == u->rx_taken && pos == u->rx_read && remaining != 0u) {
        return;
    }
    if (!mclw_hal_try_lock()) {
        return;
    }
    if (!u->active) {
        mclw_hal_unlock();
        return;
    }
    // The count moves when a read is issued, the write address when the
    // byte has landed; the ring offset is exact, the count says how many
    // laps it has made. The two differ by the bytes in flight at most.
    remaining = ch->transfer_count;
    issued = UART_PIO_RX_DMA_COUNT - remaining;
    pos = (ch->write_addr - (uint32_t)(uintptr_t)u->rx_ring) & UART_PIO_RX_RING_MASK;
    uint32_t delta = (pos - u->rx_base - issued) & UART_PIO_RX_RING_MASK;
    uint32_t written = delta < UART_PIO_RX_RING_SIZE / 2u ? issued + delta
        : issued - (UART_PIO_RX_RING_SIZE - delta);
    uint32_t avail = written - u->rx_taken;
    bool deliver = false;

    if (avail != 0u) {
        uint32_t state = mclw_hal_critical_enter();
        if (avail > UART_PIO_RX_RING_SIZE - UART_PIO_RX_SLACK) {
            u->base.stats.overruns += avail;
        } else {
            u->base.stats.bytes_rx += avail;
            deliver = u->base.on_rx != NULL;
        }
        mclw_hal_critical_exit(state);
        if (deliver) {
            for (uint32_t i = 0; i < avail; i++) {
                u->rx_linear[i] = u->rx_ring[(u->rx_read + i) & UART_PIO_RX_RING_MASK];
            }
        }
        u->rx_read = pos;
        u->rx_taken = written;
    }
    if (remaining == 0u && u->rx_taken == UART_PIO_RX_DMA_COUNT && !dma_channel_is_busy(u->rx_dma)) {
        u->rx_base = u->rx_read;
        u->rx_taken = 0;
        dma_channel_set_trans_count(u->rx_dma, UART_PIO_RX_DMA_COUNT, true);
    }
    if (deliver) {
        u->base.on_rx(&u->base, u->rx_linear, avail, u->base.on_rx_ctx);
    }
    mclw_hal_unlock();
}

static void uart_pio_poll(void *ctx) {
    (void)ctx;
    for (size_t i = 0; i < UART_PIO_COUNT; i++) {
        struct uart_pio *u = &slots[i];
        uint32_t state = mclw_hal_critical_enter();

        if (!u->active) {
            mclw_hal_critical_exit(state);
            continue;
        }
        tx_service(u);
        uint32_t frame_flag = 1u << (UART_PIO_FRAME_FLAG + u->rx_sm);
        if ((u->pio->irq & frame_flag) != 0u) {
            u->pio->irq = frame_flag;
            u->base.stats.frame_errors++;
        }
        // A receiver that stalls on a full FIFO misses the next start bit:
        // the DMA fell behind.
        uint32_t stall = 1u << (PIO_FDEBUG_RXSTALL_LSB + u->rx_sm);
        if ((u->pio->fdebug & stall) != 0u) {
            u->pio->fdebug = stall;
            u->base.stats.overruns++;
        }
        mclw_hal_critical_exit(state);
        rx_deliver(u);
    }
}

// Programs are shared by every UART on one PIO; the first loads them and
// the last unloads them. PIO0 is tried first: PIO1 is where the SPI target
// engine needs all of its instruction memory.
static int acquire_pio(struct uart_pio *u, uint8_t tx, uint8_t rx) {
    int err = -ENOSPC;

    for (uint32_t i = 0; i < NUM_PIOS; i++) {
        PIO pio = pio_get_instance(i);
        struct pio_programs *p = &programs[i];

        if ((p->users == 0u && !pio_can_add_program(pio, &tx_program)) ||
            !mclw_pio_window(pio, MCLW_PIO_PIN(tx) | MCLW_PIO_PIN(rx))) {
            continue;
        }
        int tx_sm = pio_claim_unused_sm(pio, false);
        if (tx_sm < 0) {
            continue;
        }
        int rx_sm = pio_claim_unused_sm(pio, false);
        if (rx_sm < 0) {
            pio_sm_unclaim(pio, (uint)tx_sm);
            continue;
        }
        if (p->users == 0u) {
            uint tx_offset = pio_add_program(pio, &tx_program);
            if (!pio_can_add_program(pio, &rx_program)) {
                pio_remove_program(pio, &tx_program, tx_offset);
                pio_sm_unclaim(pio, (uint)tx_sm);
                pio_sm_unclaim(pio, (uint)rx_sm);
                continue;
            }
            p->tx_offset = (uint8_t)tx_offset;
            p->rx_offset = (uint8_t)pio_add_program(pio, &rx_program);
        }
        p->users++;
        u->pio = pio;
        u->pio_index = (uint8_t)i;
        u->tx_sm = (uint8_t)tx_sm;
        u->rx_sm = (uint8_t)rx_sm;
        return 0;
    }
    return err;
}

static void release_pio(struct uart_pio *u) {
    struct pio_programs *p = &programs[u->pio_index];

    pio_sm_set_enabled(u->pio, u->tx_sm, false);
    pio_sm_set_enabled(u->pio, u->rx_sm, false);
    pio_sm_unclaim(u->pio, u->tx_sm);
    pio_sm_unclaim(u->pio, u->rx_sm);
    if (--p->users == 0u) {
        pio_remove_program(u->pio, &tx_program, p->tx_offset);
        pio_remove_program(u->pio, &rx_program, p->rx_offset);
    }
}

static void start_machines(struct uart_pio *u, uint32_t div256) {
    PIO pio = u->pio;
    const struct pio_programs *p = &programs[u->pio_index];
    pio_sm_config c;

    gpio_pull_up(u->rx_pin);
    pio_gpio_init(pio, u->rx_pin);
    pio_sm_set_consecutive_pindirs(pio, u->rx_sm, u->rx_pin, 1, false);
    c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, p->rx_offset, p->rx_offset + RX_PUSH);
    sm_config_set_in_pins(&c, u->rx_pin);
    sm_config_set_jmp_pin(&c, u->rx_pin);
    sm_config_set_in_shift(&c, true, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv_int_frac8(&c, div256 >> 8, (uint8_t)(div256 & 0xffu));
    pio_sm_init(pio, u->rx_sm, p->rx_offset, &c);

    // The line idles high before PIO takes the pin, so the DUT never sees
    // a start bit from the hand-over.
    pio_sm_set_pins_with_mask64(pio, u->tx_sm, MCLW_PIO_PIN(u->tx_pin), MCLW_PIO_PIN(u->tx_pin));
    pio_sm_set_pindirs_with_mask64(pio, u->tx_sm, MCLW_PIO_PIN(u->tx_pin), MCLW_PIO_PIN(u->tx_pin));
    pio_gpio_init(pio, u->tx_pin);
    c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, p->tx_offset, tx_wrap(u));
    sm_config_set_sideset(&c, 2, true, false);
    sm_config_set_out_pins(&c, u->tx_pin, 1);
    sm_config_set_sideset_pins(&c, u->tx_pin);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv_int_frac8(&c, div256 >> 8, (uint8_t)(div256 & 0xffu));
    pio_sm_init(pio, u->tx_sm, p->tx_offset, &c);

    pio->irq = 1u << (UART_PIO_FRAME_FLAG + u->rx_sm);
    pio->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + u->rx_sm);

    // Receive: the byte the program pushed sits in the top lane of the FIFO
    // word; the ring wraps in hardware. Transmit: a byte write to the FIFO
    // is replicated across the word and the program shifts out the low 8.
    dma_channel_config rx = dma_channel_get_default_config(u->rx_dma);
    channel_config_set_transfer_data_size(&rx, DMA_SIZE_8);
    channel_config_set_read_increment(&rx, false);
    channel_config_set_write_increment(&rx, true);
    channel_config_set_ring(&rx, true, UART_PIO_RX_RING_BITS);
    channel_config_set_dreq(&rx, pio_get_dreq(pio, u->rx_sm, false));
    dma_channel_configure(u->rx_dma, &rx, u->rx_ring, (const volatile uint8_t *)&pio->rxf[u->rx_sm] + 3,
        UART_PIO_RX_DMA_COUNT, true);

    dma_channel_config tx = dma_channel_get_default_config(u->tx_dma);
    channel_config_set_transfer_data_size(&tx, DMA_SIZE_8);
    channel_config_set_read_increment(&tx, true);
    channel_config_set_write_increment(&tx, false);
    channel_config_set_ring(&tx, false, UART_PIO_TX_RING_BITS);
    channel_config_set_dreq(&tx, pio_get_dreq(pio, u->tx_sm, true));
    dma_channel_configure(u->tx_dma, &tx, &pio->txf[u->tx_sm], u->tx_ring, 0, false);

    pio_enable_sm_mask_in_sync(pio, (1u << u->tx_sm) | (1u << u->rx_sm));
}

// Disabled before the abort so nothing can re-trigger it.
static void stop_channel(uint channel) {
    hw_clear_bits(&dma_channel_hw_addr(channel)->al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(channel);
}

static void uart_pio_close(struct mclw_uart *uart) {
    struct uart_pio *u = (struct uart_pio *)uart;
    uint32_t state = mclw_hal_critical_enter();

    u->active = false;
    mclw_hal_critical_exit(state);
    pio_sm_set_enabled(u->pio, u->tx_sm, false);
    pio_sm_set_enabled(u->pio, u->rx_sm, false);
    stop_channel(u->tx_dma);
    stop_channel(u->rx_dma);
    dma_channel_unclaim(u->tx_dma);
    dma_channel_unclaim(u->rx_dma);
    release_pio(u);
    gpio_set_pulls(u->rx_pin, false, false);
    gpio_deinit(u->tx_pin);
    gpio_deinit(u->rx_pin);
    mclw_hal_pin_release(u->tx_pin, u->base.owner);
    mclw_hal_pin_release(u->rx_pin, u->base.owner);
    u->base.owner = NULL;
    u->base.on_rx = NULL;
    u->base.on_rx_ctx = NULL;
}

static int uart_pio_set_baud(struct mclw_uart *uart, uint32_t baud) {
    struct uart_pio *u = (struct uart_pio *)uart;
    uint32_t div256;
    uint32_t actual;

    if (!divider_for(baud, &div256, &actual)) {
        return -EINVAL;
    }
    int err = tx_drain(u);
    if (err != 0) {
        return err;
    }
    apply_divider(u, div256);
    u->base.baud = actual;
    return 0;
}

static int uart_pio_set_format(struct mclw_uart *uart, const struct mclw_uart_format *format) {
    struct uart_pio *u = (struct uart_pio *)uart;

    if (!format_supported(format)) {
        return -ENOTSUP;
    }
    int err = tx_drain(u);
    if (err != 0) {
        return err;
    }
    u->stop_bits = format->stop_bits;
    pio_sm_set_wrap(u->pio, u->tx_sm, programs[u->pio_index].tx_offset, tx_wrap(u));
    return 0;
}

static size_t uart_pio_send(struct mclw_uart *uart, const uint8_t *bytes, size_t len) {
    struct uart_pio *u = (struct uart_pio *)uart;
    uint32_t head = u->tx_head;
    uint32_t free_slots = UART_PIO_TX_RING_SIZE - (head - u->tx_tail);
    size_t queued = len < free_slots ? len : free_slots;

    if (queued == 0u) {
        return 0;
    }
    for (size_t i = 0; i < queued; i++) {
        u->tx_ring[(head + i) & UART_PIO_TX_RING_MASK] = bytes[i];
    }
    __dmb();
    u->tx_head = head + (uint32_t)queued;

    uint32_t state = mclw_hal_critical_enter();
    tx_service(u);
    mclw_hal_critical_exit(state);
    return queued;
}

static const struct mclw_uart_engine uart_pio_engine = {
    .close = uart_pio_close,
    .set_baud = uart_pio_set_baud,
    .set_format = uart_pio_set_format,
    .send = uart_pio_send,
};

struct mclw_uart *mclw_uart_pio_open(uint8_t tx, uint8_t rx, uint32_t baud,
    const struct mclw_uart_format *format, const void *owner, int *err) {
    struct uart_pio *u = NULL;
    size_t index = 0;
    uint32_t div256;
    uint32_t actual;

    if (!format_supported(format)) {
        *err = -ENOTSUP;
        return NULL;
    }
    if (!divider_for(baud, &div256, &actual)) {
        *err = -EINVAL;
        return NULL;
    }
    for (index = 0; index < UART_PIO_COUNT; index++) {
        if (slots[index].base.owner == NULL) {
            u = &slots[index];
            break;
        }
    }
    if (u == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    if (!poll_registered) {
        if (mclw_hal_poll_register(uart_pio_poll, NULL) != 0) {
            *err = -ENOSPC;
            return NULL;
        }
        poll_registered = true;
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
    int tx_dma = dma_claim_unused_channel(false);
    int rx_dma = tx_dma < 0 ? -1 : dma_claim_unused_channel(false);
    if (rx_dma < 0) {
        if (tx_dma >= 0) {
            dma_channel_unclaim((uint)tx_dma);
        }
        mclw_hal_pin_release(tx, owner);
        mclw_hal_pin_release(rx, owner);
        *err = -ENOMEM;
        return NULL;
    }
    if (tx_instructions[TX_STOP] == 0u) {
        encode_programs();
    }
    *err = acquire_pio(u, tx, rx);
    if (*err != 0) {
        dma_channel_unclaim((uint)tx_dma);
        dma_channel_unclaim((uint)rx_dma);
        mclw_hal_pin_release(tx, owner);
        mclw_hal_pin_release(rx, owner);
        return NULL;
    }

    u->base.engine = &uart_pio_engine;
    u->base.baud = actual;
    u->base.on_rx = NULL;
    u->base.on_rx_ctx = NULL;
    u->base.stats.bytes_rx = 0;
    u->base.stats.bytes_tx = 0;
    u->base.stats.overruns = 0;
    u->base.stats.frame_errors = 0;
    u->tx_pin = tx;
    u->rx_pin = rx;
    u->stop_bits = format->stop_bits;
    u->tx_dma = (uint8_t)tx_dma;
    u->rx_dma = (uint8_t)rx_dma;
    u->tx_ring = tx_rings[index];
    u->rx_ring = rx_rings[index];
    u->tx_head = 0;
    u->tx_tail = 0;
    u->tx_inflight = 0;
    u->rx_base = 0;
    u->rx_taken = 0;
    u->rx_read = 0;
    start_machines(u, div256);

    uint32_t state = mclw_hal_critical_enter();
    u->base.owner = owner;
    u->active = true;
    mclw_hal_critical_exit(state);
    *err = 0;
    return &u->base;
}
