// SPDX-License-Identifier: Apache-2.0 OR MIT
// The HAL engines the host port does not have yet: opening one fails with
// -ENOTSUP, so a model that needs it refuses to attach and says so. Each
// moves to a file of its own as the bridge learns its bus (BRIDGE.md).

#include <errno.h>
#include <stddef.h>

#include "metalclaw/hal/spi_peer.h"
#include "metalclaw/hal/spi_sink.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/hal/swd.h"
#include "metalclaw/hal/uart.h"
#include "metalclaw/hal/usb.h"
#include "metalclaw/hal/wire.h"

// ---- SPI target -------------------------------------------------------------

struct mclw_spi_target *mclw_hal_spi_target_open(uint8_t sck, uint8_t mosi, uint8_t miso, uint8_t cs,
    uint8_t mode, bool msb_first, const void *owner, int *err) {
    (void)sck;
    (void)mosi;
    (void)miso;
    (void)cs;
    (void)mode;
    (void)msb_first;
    (void)owner;
    *err = -ENOTSUP;
    return NULL;
}

void mclw_hal_spi_target_close(struct mclw_spi_target *t) {
    (void)t;
}

int mclw_hal_spi_target_set_mode(struct mclw_spi_target *t, uint8_t mode) {
    (void)t;
    (void)mode;
    return -ENOTSUP;
}

uint8_t mclw_hal_spi_target_mode(const struct mclw_spi_target *t) {
    (void)t;
    return 0;
}

int mclw_hal_spi_target_arm_regmap(struct mclw_spi_target *t, struct mclw_regmap *map,
    const struct mclw_regmap_framing *framing) {
    (void)t;
    (void)map;
    (void)framing;
    return -ENOTSUP;
}

int mclw_hal_spi_target_arm_stream(struct mclw_spi_target *t, struct mclw_stream *stream) {
    (void)t;
    (void)stream;
    return -ENOTSUP;
}

int mclw_hal_spi_target_arm_command(struct mclw_spi_target *t, struct mclw_command_image *image,
    struct mclw_gpio *busy) {
    (void)t;
    (void)image;
    (void)busy;
    return -ENOTSUP;
}

void mclw_hal_spi_target_set_busy(struct mclw_spi_target *t, bool busy) {
    (void)t;
    (void)busy;
}

void mclw_hal_spi_target_disarm(struct mclw_spi_target *t) {
    (void)t;
}

void mclw_hal_spi_target_poll(void) {
}

void mclw_hal_spi_target_stats(const struct mclw_spi_target *t, struct mclw_spi_target_stats *out) {
    (void)t;
    *out = (struct mclw_spi_target_stats){ 0 };
}

uint32_t mclw_hal_spi_target_max_clock_hz(const struct mclw_spi_target *t) {
    (void)t;
    return 0;
}

// ---- SPI sink ----------------------------------------------------------------

struct mclw_spi_sink *mclw_hal_spi_sink_open(uint8_t sck, uint8_t mosi, uint8_t cs, uint8_t line,
    uint8_t mode, bool msb_first, const void *owner, int *err) {
    (void)sck;
    (void)mosi;
    (void)cs;
    (void)line;
    (void)mode;
    (void)msb_first;
    (void)owner;
    *err = -ENOTSUP;
    return NULL;
}

void mclw_hal_spi_sink_close(struct mclw_spi_sink *s) {
    (void)s;
}

int mclw_hal_spi_sink_arm(struct mclw_spi_sink *s, struct mclw_sink *sink) {
    (void)s;
    (void)sink;
    return -ENOTSUP;
}

void mclw_hal_spi_sink_disarm(struct mclw_spi_sink *s) {
    (void)s;
}

void mclw_hal_spi_sink_stats(const struct mclw_spi_sink *s, struct mclw_spi_sink_stats *out) {
    (void)s;
    *out = (struct mclw_spi_sink_stats){ 0 };
}

uint32_t mclw_hal_spi_sink_max_clock_hz(const struct mclw_spi_sink *s) {
    (void)s;
    return 0;
}

// ---- SPI peer ----------------------------------------------------------------

struct mclw_spi_peer *mclw_hal_spi_peer_target(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, int *err) {
    (void)sck;
    (void)mosi;
    (void)miso;
    (void)cs;
    *err = -ENOTSUP;
    return NULL;
}

struct mclw_spi_peer *mclw_hal_spi_peer_controller(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, uint32_t hz, int *err) {
    (void)sck;
    (void)mosi;
    (void)miso;
    (void)cs;
    (void)hz;
    *err = -ENOTSUP;
    return NULL;
}

void mclw_hal_spi_peer_close(struct mclw_spi_peer *peer) {
    (void)peer;
}

void mclw_hal_spi_peer_load(struct mclw_spi_peer *peer, const uint8_t *bytes, size_t len) {
    (void)peer;
    (void)bytes;
    (void)len;
}

void mclw_hal_spi_peer_received(const struct mclw_spi_peer *peer, size_t at, uint8_t *bytes, size_t len) {
    (void)peer;
    (void)at;
    (void)bytes;
    (void)len;
}

void mclw_hal_spi_peer_restart(struct mclw_spi_peer *peer) {
    (void)peer;
}

int mclw_hal_spi_peer_transfer(struct mclw_spi_peer *peer, const uint8_t *out, uint8_t *in, size_t len) {
    (void)peer;
    (void)out;
    (void)in;
    (void)len;
    return -ENOTSUP;
}

bool mclw_hal_spi_peer_busy(const struct mclw_spi_peer *peer) {
    (void)peer;
    return false;
}

uint32_t mclw_hal_spi_peer_count(const struct mclw_spi_peer *peer) {
    (void)peer;
    return 0;
}

// ---- UART --------------------------------------------------------------------

struct mclw_uart *mclw_hal_uart_open(uint8_t tx, uint8_t rx, uint32_t baud,
    const struct mclw_uart_format *format, const void *owner, int *err) {
    (void)tx;
    (void)rx;
    (void)baud;
    (void)format;
    (void)owner;
    *err = -ENOTSUP;
    return NULL;
}

void mclw_hal_uart_close(struct mclw_uart *uart) {
    (void)uart;
}

int mclw_hal_uart_set_baud(struct mclw_uart *uart, uint32_t baud) {
    (void)uart;
    (void)baud;
    return -ENOTSUP;
}

uint32_t mclw_hal_uart_baud(const struct mclw_uart *uart) {
    (void)uart;
    return 0;
}

int mclw_hal_uart_set_format(struct mclw_uart *uart, const struct mclw_uart_format *format) {
    (void)uart;
    (void)format;
    return -ENOTSUP;
}

void mclw_hal_uart_set_rx(struct mclw_uart *uart, mclw_uart_rx_t fn, void *ctx) {
    (void)uart;
    (void)fn;
    (void)ctx;
}

size_t mclw_hal_uart_send(struct mclw_uart *uart, const uint8_t *bytes, size_t len) {
    (void)uart;
    (void)bytes;
    (void)len;
    return 0;
}

void mclw_hal_uart_stats(const struct mclw_uart *uart, struct mclw_uart_stats *out) {
    (void)uart;
    *out = (struct mclw_uart_stats){ 0 };
}

// ---- Wire --------------------------------------------------------------------

struct mclw_wire *mclw_hal_wire_open(const uint8_t *pins, uint8_t count, const struct mclw_wire_facts *facts,
    const void *owner, int *err) {
    (void)pins;
    (void)count;
    (void)facts;
    (void)owner;
    *err = -ENOTSUP;
    return NULL;
}

void mclw_hal_wire_close(struct mclw_wire *w) {
    (void)w;
}

void mclw_hal_wire_stats(const struct mclw_wire *w, struct mclw_wire_stats *out) {
    (void)w;
    *out = (struct mclw_wire_stats){ 0 };
}

uint8_t mclw_hal_wire_pin(const struct mclw_wire *w, uint8_t index) {
    (void)w;
    (void)index;
    return 0;
}

int mclw_hal_wire_sample_bit(const struct mclw_wire *w, uint8_t pin) {
    (void)w;
    (void)pin;
    return -1;
}

size_t mclw_hal_wire_read(struct mclw_wire *w, uint32_t *out, size_t max) {
    (void)w;
    (void)out;
    (void)max;
    return 0;
}

int mclw_hal_wire_capture(struct mclw_wire *w) {
    (void)w;
    return -ENOTSUP;
}

size_t mclw_hal_wire_capture_words(const struct mclw_wire *w) {
    (void)w;
    return 0;
}

int mclw_hal_wire_respond(struct mclw_wire *w, const uint32_t *pulses, size_t count) {
    (void)w;
    (void)pulses;
    (void)count;
    return -ENOTSUP;
}

uint32_t mclw_hal_wire_responses(struct mclw_wire *w) {
    (void)w;
    return 0;
}

int mclw_hal_wire_set_period(struct mclw_wire *w, uint32_t ns) {
    (void)w;
    (void)ns;
    return -ENOTSUP;
}

size_t mclw_hal_wire_write(struct mclw_wire *w, const uint32_t *words, size_t count) {
    (void)w;
    (void)words;
    (void)count;
    return 0;
}

bool mclw_hal_wire_idle(struct mclw_wire *w) {
    (void)w;
    return true;
}

int mclw_hal_wire_onewire_arm(struct mclw_wire *w, struct mclw_onewire *image) {
    (void)w;
    (void)image;
    return -ENOTSUP;
}

// ---- SWD ---------------------------------------------------------------------

struct mclw_swd *mclw_hal_swd_open(uint8_t swclk, uint8_t swdio, uint8_t nreset, const void *owner, int *err) {
    (void)swclk;
    (void)swdio;
    (void)nreset;
    (void)owner;
    *err = -ENOTSUP;
    return NULL;
}

void mclw_hal_swd_close(struct mclw_swd *swd) {
    (void)swd;
}

int mclw_hal_swd_port_on(struct mclw_swd *swd) {
    (void)swd;
    return -ENOTSUP;
}

int mclw_hal_swd_port_off(struct mclw_swd *swd) {
    (void)swd;
    return -ENOTSUP;
}

int mclw_hal_swd_set_clock(struct mclw_swd *swd, uint32_t hz) {
    (void)swd;
    (void)hz;
    return -ENOTSUP;
}

uint32_t mclw_hal_swd_get_clock(const struct mclw_swd *swd) {
    (void)swd;
    return 0;
}

int mclw_hal_swd_configure(struct mclw_swd *swd, uint8_t turnaround, bool data_phase) {
    (void)swd;
    (void)turnaround;
    (void)data_phase;
    return -ENOTSUP;
}

int mclw_hal_swd_output_sequence(struct mclw_swd *swd, uint32_t count, const uint8_t *data) {
    (void)swd;
    (void)count;
    (void)data;
    return -ENOTSUP;
}

int mclw_hal_swd_input_sequence(struct mclw_swd *swd, uint32_t count, uint8_t *data) {
    (void)swd;
    (void)count;
    (void)data;
    return -ENOTSUP;
}

int mclw_hal_swd_transfer(struct mclw_swd *swd, uint8_t request, uint32_t *data, uint8_t idle_cycles,
    uint8_t *response) {
    (void)swd;
    (void)request;
    (void)data;
    (void)idle_cycles;
    (void)response;
    return -ENOTSUP;
}

int mclw_hal_swd_set_pins(struct mclw_swd *swd, uint8_t mask, uint8_t values) {
    (void)swd;
    (void)mask;
    (void)values;
    return -ENOTSUP;
}

int mclw_hal_swd_get_pins(struct mclw_swd *swd, uint8_t *values) {
    (void)swd;
    (void)values;
    return -ENOTSUP;
}

// ---- USB ---------------------------------------------------------------------

struct mclw_usb_interface *mclw_hal_usb_open(const struct mclw_usb_interface_desc *desc,
    mclw_usb_rx_t rx, mclw_usb_control_t control, void *ctx, const void *owner, int *err) {
    (void)desc;
    (void)rx;
    (void)control;
    (void)ctx;
    (void)owner;
    *err = -ENOTSUP;
    return NULL;
}

void mclw_hal_usb_close(struct mclw_usb_interface *itf) {
    (void)itf;
}

size_t mclw_hal_usb_write(struct mclw_usb_interface *itf, const uint8_t *data, size_t len) {
    (void)itf;
    (void)data;
    (void)len;
    return 0;
}

bool mclw_hal_usb_connected(const struct mclw_usb_interface *itf) {
    (void)itf;
    return false;
}

int mclw_hal_usb_line_coding(const struct mclw_usb_interface *itf, struct mclw_usb_line_coding *out) {
    (void)itf;
    (void)out;
    return -ENOTSUP;
}

uint8_t mclw_hal_usb_control_lines(const struct mclw_usb_interface *itf) {
    (void)itf;
    return 0;
}

const char *mclw_hal_usb_serial(void) {
    return "host";
}
