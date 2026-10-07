// SPDX-License-Identifier: Apache-2.0 OR MIT
// A vendor bulk interface on the composite USB device, owned by a device
// instance. Opening or closing one changes the interface set; the port
// re-enumerates after a short debounce so several changes cost one.

#ifndef MCLW_HAL_USB_H
#define MCLW_HAL_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mclw_usb_interface;

// A vendor-class bulk interface; the name is the interface string hosts
// such as probe-rs match on.
struct mclw_usb_interface_desc {
    const char *name;
};

// Called from the USB task context with each received packet. The handler
// answers through mclw_hal_usb_write before returning.
typedef void (*mclw_usb_rx_t)(struct mclw_usb_interface *itf, const uint8_t *data,
    size_t len, void *ctx);

struct mclw_usb_interface *mclw_hal_usb_open(const struct mclw_usb_interface_desc *desc,
    mclw_usb_rx_t rx, void *ctx, const void *owner, int *err);
void mclw_hal_usb_close(struct mclw_usb_interface *itf);

// Queues up to `len` bytes for the IN endpoint; returns how many were taken.
size_t mclw_hal_usb_write(struct mclw_usb_interface *itf, const uint8_t *data, size_t len);
bool mclw_hal_usb_connected(const struct mclw_usb_interface *itf);

// The device's USB serial string, for protocols that echo it (CMSIS-DAP).
const char *mclw_hal_usb_serial(void);

#endif
