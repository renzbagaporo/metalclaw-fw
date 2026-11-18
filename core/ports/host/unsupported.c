// SPDX-License-Identifier: Apache-2.0 OR MIT
// The HAL engines the host port does not have yet: opening one fails with
// -ENOTSUP, so a model that needs it refuses to attach and says so. Each
// moves to a file of its own as the bridge learns its bus (BRIDGE.md).

#include <errno.h>
#include <stddef.h>

#include "metalclaw/hal/spi_peer.h"
#include "metalclaw/hal/spi_sink.h"
#include "metalclaw/hal/swd.h"
#include "metalclaw/hal/usb.h"

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
