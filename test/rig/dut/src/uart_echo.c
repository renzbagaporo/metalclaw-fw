// SPDX-License-Identifier: Apache-2.0 OR MIT

/*
 * An image whose devicetree has an `echo-uart` alias sends every byte that
 * UART receives straight back for as long as it runs: whatever reaches the
 * DUT on that UART returns unchanged, at line rate in both directions. No
 * command starts or stops it.
 *
 * The interrupt only receives. Sending runs in a thread below main's
 * priority with uart_poll_out(): the PL011 driver's uart_irq_tx_enable()
 * calls the interrupt callback in a loop from wherever it is called, so
 * starting transmission from the callback would re-enter it.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#if DT_NODE_EXISTS(DT_ALIAS(echo_uart)) && defined(CONFIG_UART_INTERRUPT_DRIVEN)
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include <stdint.h>

#define ECHO_RING_SIZE 4096U
#define ECHO_CHUNK 32U
#define ECHO_STACK_SIZE 1024
#define ECHO_PRIORITY 7

static const struct device *const echo_dev = DEVICE_DT_GET(DT_ALIAS(echo_uart));
RING_BUF_DECLARE(echo_ring, ECHO_RING_SIZE);
static K_SEM_DEFINE(echo_ready, 0, 1);

/* One producer (this interrupt) and one consumer (the thread) need no lock. */
static void echo_isr(const struct device *dev, void *user_data)
{
	(void)user_data;
	while (uart_irq_update(dev) > 0 && uart_irq_rx_ready(dev) > 0) {
		uint8_t chunk[ECHO_CHUNK];
		int n = uart_fifo_read(dev, chunk, sizeof(chunk));

		if (n <= 0) {
			break;
		}
		(void)ring_buf_put(&echo_ring, chunk, (uint32_t)n);
		k_sem_give(&echo_ready);
	}
}

static void echo_thread(void *a, void *b, void *c)
{
	(void)a;
	(void)b;
	(void)c;
	if (!device_is_ready(echo_dev) ||
		uart_irq_callback_user_data_set(echo_dev, echo_isr, NULL) != 0) {
		return;
	}
	uart_irq_rx_enable(echo_dev);
	for (;;) {
		uint8_t chunk[ECHO_CHUNK];
		uint32_t n;

		(void)k_sem_take(&echo_ready, K_FOREVER);
		while ((n = ring_buf_get(&echo_ring, chunk, sizeof(chunk))) > 0U) {
			for (uint32_t i = 0; i < n; i++) {
				uart_poll_out(echo_dev, chunk[i]);
			}
		}
	}
}

K_THREAD_DEFINE(echo_tid, ECHO_STACK_SIZE, echo_thread, NULL, NULL, NULL, ECHO_PRIORITY, 0, 0);
#endif
