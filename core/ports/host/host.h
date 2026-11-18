// SPDX-License-Identifier: Apache-2.0 OR MIT
// The host port: Metalclaw as a Linux process. Device models, the runtime
// and the MicroPython binding are the firmware's own; this port implements
// the HAL in software, on one worker thread.
//
// Two modes, chosen at start-up by the environment:
//   - standalone (METALCLAW_BRIDGE unset): time is the process's monotonic
//     clock and the worker ticks once a millisecond of it;
//   - bridged (METALCLAW_BRIDGE=<unix socket path>): an emulator holding
//     the DUT connects to that socket (bridge.c, BRIDGE.md). Time is then
//     the DUT's virtual time, carried by every request, and the worker runs
//     only when a request arrives or work is posted, so the models see the
//     DUT's clock and nothing else.

#ifndef MCLW_PORT_HOST_H
#define MCLW_PORT_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// platform.c
bool mclw_host_bridged(void);
// Bridged: the DUT's virtual time moves to `ns` (never back), then the
// worker runs one round: posted work, then every poll (the runtime's tick
// among them).
void mclw_host_advance(uint64_t ns);
// The virtual time, in ns, an output event made now happens at: the
// request's time plus whatever mclw_hal_delay_us added since.
uint64_t mclw_host_event_ns(void);
// Runs posted work only, at the current time.
void mclw_host_drain_work(void);
// Empties the wake pipe `fd` once it has been seen readable.
void mclw_host_drain_wake(int fd);

// bridge.c: serves the emulator on the worker thread until it
// disconnects; `wake_fd` becomes readable when work is posted.
void mclw_host_bridge_run(const char *path, int wake_fd);
// Events toward the DUT, sent with the reply to the request being handled,
// or with the next reply when made outside one.
#define MCLW_HOST_GPIO_LOW 0u
#define MCLW_HOST_GPIO_HIGH 1u
#define MCLW_HOST_GPIO_RELEASED 2u
// A GPIO action at mclw_host_event_ns(); a UART byte toward the DUT's
// receive pin `rx_pin`, at `t_ns`, which the UART paces at its baud rate.
void mclw_host_event_gpio(uint8_t pin, uint8_t state);
void mclw_host_event_uart(uint8_t rx_pin, uint8_t byte, uint64_t t_ns);

// gpio.c: the level the DUT drives on `pin`, 0 or 1, or -1 for released.
void mclw_host_gpio_dut(uint8_t pin, int level);

// uart.c: bytes the DUT transmitted on `dut_tx_pin`, for the UART that
// receives on it.
void mclw_host_uart_receive(uint8_t dut_tx_pin, const uint8_t *bytes, size_t len);

// i2c_target.c, called by the bridge on the worker thread, one bus event
// at a time as the DUT's controller makes them. start and write return
// whether the target acknowledges; a START to an address no target
// answers is not acknowledged.
bool mclw_host_i2c_start(uint8_t sda, uint8_t scl, uint8_t address, bool read);
bool mclw_host_i2c_write(uint8_t sda, uint8_t scl, uint8_t byte);
uint8_t mclw_host_i2c_read(uint8_t sda, uint8_t scl);
void mclw_host_i2c_stop(uint8_t sda, uint8_t scl);

#endif
