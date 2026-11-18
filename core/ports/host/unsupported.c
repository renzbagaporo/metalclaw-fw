// SPDX-License-Identifier: Apache-2.0 OR MIT
// The HAL engines the host port does not have yet: opening one fails with
// -ENOTSUP, so a model that needs it refuses to attach and says so. Each
// moves to a file of its own as the bridge learns its bus (BRIDGE.md).

#include <errno.h>
#include <stddef.h>

#include "metalclaw/hal/swd.h"
#include "metalclaw/hal/usb.h"

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
