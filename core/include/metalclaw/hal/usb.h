// SPDX-License-Identifier: Apache-2.0 OR MIT
// A vendor bulk interface on the composite USB device, owned by a device
// instance. Opening or closing one changes the interface set; the port
// re-enumerates after a short debounce so several changes cost one.

#ifndef METALCLAW_HAL_USB_H
#define METALCLAW_HAL_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct metalclaw_usb_interface;

struct metalclaw_usb_interface_desc {
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    // Shown as the interface string; hosts such as probe-rs match on it.
    const char *name;
};

// Called from the USB task context on core 0 with each received packet.
// The handler answers through metalclaw_hal_usb_write before returning.
typedef void (*metalclaw_usb_rx_t)(struct metalclaw_usb_interface *itf, const uint8_t *data,
    size_t len, void *ctx);

struct metalclaw_usb_interface *metalclaw_hal_usb_open(const struct metalclaw_usb_interface_desc *desc,
    metalclaw_usb_rx_t rx, void *ctx, const void *owner, int *err);
void metalclaw_hal_usb_close(struct metalclaw_usb_interface *itf);

// Queues up to `len` bytes for the IN endpoint; returns how many were taken.
size_t metalclaw_hal_usb_write(struct metalclaw_usb_interface *itf, const uint8_t *data, size_t len);
bool metalclaw_hal_usb_connected(const struct metalclaw_usb_interface *itf);

// The device's USB serial string, for protocols that echo it (CMSIS-DAP).
const char *metalclaw_hal_usb_serial(void);

#endif
