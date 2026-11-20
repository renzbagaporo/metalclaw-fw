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
// A GPIO action at `t_ns`, for an engine timing the line itself.
void mclw_host_event_gpio_at(uint8_t pin, uint8_t state, uint64_t t_ns);
// Asks the emulator to report every level the DUT drives on `pin`.
void mclw_host_event_watch(uint8_t pin);
void mclw_host_event_uart(uint8_t rx_pin, uint8_t byte, uint64_t t_ns);

// gpio.c: the level the DUT drives on `pin`, 0 or 1, or -1 for released.
void mclw_host_gpio_dut(uint8_t pin, int level);

// uart.c: bytes the DUT transmitted on `dut_tx_pin`, for the UART that
// receives on it.
void mclw_host_uart_receive(uint8_t dut_tx_pin, const uint8_t *bytes, size_t len);
// A level the DUT drove on a pin, for a bit-level UART that receives on it.
void mclw_host_uart_edge(uint8_t pin, int level, uint64_t t_ns);
// The DUT's UARTs moved: a line that now carries bits is watched.
void mclw_host_uart_route_changed(void);
// Ends the bit-level frames that are over by `now_ns`.
void mclw_host_uart_poll(uint64_t now_ns);
// bridge.c: whether a bridged UART block of the DUT's has `pin` as its TX,
// or RX, now: its bytes go whole.
bool mclw_host_pin_is_uart_tx(uint8_t pin);
bool mclw_host_pin_is_uart_rx(uint8_t pin);

// spi_target.c: the DUT's chip select on `pin`, and one frame on the bus
// at `sck` (-ENODEV when no target there is selected).
void mclw_host_spi_cs(uint8_t pin, bool level);
int mclw_host_spi_frame(uint8_t sck, uint8_t mosi_pin, uint8_t miso_pin, uint8_t mosi, uint8_t *miso);

// wire.c: the DUT drove `pin` to `level` (0, 1, or -1 for released) at
// `t_ns`.
void mclw_host_wire_edge(uint8_t pin, int level, uint64_t t_ns);

// spi_sink.c: the same for receive-only targets; frame and cs say whether
// a sink took them. `line` is a sink's side line, as the DUT drives it.
bool mclw_host_spi_sink_cs(uint8_t pin, bool level);
bool mclw_host_spi_sink_frame(uint8_t sck, uint8_t mosi_pin, uint8_t mosi);
// A whole SPI frame's byte, MSB first, into every CLOCKED_IN wire on that
// clock and data pin (an APA102 the DUT drives from a SPI controller);
// false when none is there.
bool mclw_host_wire_spi_frame(uint8_t sck, uint8_t mosi_pin, uint8_t mosi);
void mclw_host_spi_sink_line(uint8_t pin, int level);

// spi_peer.c: a frame for a target peer on that bus; false when none.
bool mclw_host_spi_peer_frame(uint8_t sck, uint8_t mosi_pin, uint8_t miso_pin, uint8_t mosi, uint8_t *miso);
// The frame a controller peer clocked into the DUT's slave on that bus:
// what came back, or -1 when the DUT's block is no enabled slave.
void mclw_host_spi_peer_clocked(uint8_t sck, int miso);
// bridge.c: clock one frame into the DUT's SPI block on these pins at t_ns,
// as an SPI controller; false when no bridged bus has them.
// A request from the emulator at t_ns, with no event of the DUT's.
void mclw_host_event_wake(uint64_t t_ns);
// wire.c: the wire engines' timed ends (a burst's, a frame's) due by now.
void mclw_host_wire_poll(uint64_t now_ns);
// A shift-register panel's look (BRIDGE.md latch_capture, latched): asked
// of the emulator, and its answer for the wire whose clock is `clock`.
void mclw_host_event_latch_capture(uint8_t clock, uint8_t latch, uint8_t enable, uint32_t pins);
void mclw_host_wire_latched(uint8_t clock, const uint32_t *samples, size_t count, uint64_t enabled_ns, bool overflow);
// Wire two DUT pins together inside the emulator, or part them again.
void mclw_host_event_join(uint8_t a, uint8_t b, bool joined);
bool mclw_host_event_spi_clock(uint8_t sck, uint8_t mosi_pin, uint8_t miso_pin, uint8_t mosi, uint64_t t_ns);
// The harness as I2C controller (BRIDGE.md i2c_drive): one event at a
// time toward the DUT's target on the bus with these pins, each answered
// by an i2c_driven request; false when no bus has those pins.
#define MCLW_HOST_I2C_START 0u
#define MCLW_HOST_I2C_WRITE 1u
#define MCLW_HOST_I2C_READ 2u
#define MCLW_HOST_I2C_STOP 3u
bool mclw_host_event_i2c_drive(uint8_t sda, uint8_t scl, uint8_t ev, uint8_t arg, bool flag, uint64_t t_ns);
// i2c_controller.c: an i2c_driven answer, `ack` 1, 0 or -1 for none, `data`
// a read's byte or -1, at the request's time.
void mclw_host_i2c_driven(uint8_t sda, uint8_t scl, int ack, int data, uint64_t t_ns);

// i2c_target.c, called by the bridge on the worker thread, one bus event
// at a time as the DUT's controller makes them. start and write return
// whether the target acknowledges; a START to an address no target
// answers is not acknowledged.
bool mclw_host_i2c_start(uint8_t sda, uint8_t scl, uint8_t address, bool read);
bool mclw_host_i2c_write(uint8_t sda, uint8_t scl, uint8_t byte);
uint8_t mclw_host_i2c_read(uint8_t sda, uint8_t scl);
void mclw_host_i2c_stop(uint8_t sda, uint8_t scl);

#endif
