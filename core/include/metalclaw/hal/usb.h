// SPDX-License-Identifier: Apache-2.0 OR MIT
// A function on the composite USB device, owned by a device instance: a
// vendor bulk interface, or a serial port the host sees as a tty (CDC ACM).
// Opening or closing one changes the interface set; the port re-enumerates
// after a short debounce so several changes cost one.

#ifndef MCLW_HAL_USB_H
#define MCLW_HAL_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"

struct mclw_usb_interface;

enum mclw_usb_function {
    MCLW_USB_VENDOR,
    MCLW_USB_SERIAL,
};

// The name is the interface string hosts such as probe-rs match on.
struct mclw_usb_interface_desc {
    const char *name;
    enum mclw_usb_function function;
};

// Called from the USB task with what the host sent. A vendor interface
// hands over one packet and ignores the return. A serial function takes
// the return as how many bytes were consumed, keeps the rest, offers it
// again later, and holds the host off meanwhile.
typedef size_t (*mclw_usb_rx_t)(struct mclw_usb_interface *itf, const uint8_t *data,
    size_t len, void *ctx);

// Called from the USB task when the host changes a serial function's line
// coding or control lines.
typedef void (*mclw_usb_control_t)(struct mclw_usb_interface *itf, void *ctx);

struct mclw_usb_interface *mclw_hal_usb_open(const struct mclw_usb_interface_desc *desc,
    mclw_usb_rx_t rx, mclw_usb_control_t control, void *ctx, const void *owner, int *err);
void mclw_hal_usb_close(struct mclw_usb_interface *itf);

// Queues up to `len` bytes for the host; returns how many were taken.
// Vendor: from the USB task only, to answer inside rx. Serial: from any
// thread context, the worker included; fewer than `len` when the queue is
// full. A serial queue is emptied when the host opens the port (raises
// DTR): what waited for a host is not replayed to the next one.
size_t mclw_hal_usb_write(struct mclw_usb_interface *itf, const uint8_t *data, size_t len);

// Vendor: the host configured the device. Serial: the host holds the port
// open (DTR). Serial answers from any thread context.
bool mclw_hal_usb_connected(const struct mclw_usb_interface *itf);

// A serial function's line coding as the host last set it. -ENOTSUP when
// the host asked for a frame a mclw_uart_format cannot express (mark
// or space parity, 1.5 stop bits, more than 8 data bits).
struct mclw_usb_line_coding {
    uint32_t baud;
    struct mclw_uart_format format;
};

int mclw_hal_usb_line_coding(const struct mclw_usb_interface *itf, struct mclw_usb_line_coding *out);

#define MCLW_USB_LINE_DTR 0x01u
#define MCLW_USB_LINE_RTS 0x02u

// The control lines the host drives on a serial function, as
// MCLW_USB_LINE_* bits. Any thread context.
uint8_t mclw_hal_usb_control_lines(const struct mclw_usb_interface *itf);

// The device's USB serial string, for protocols that echo it (CMSIS-DAP).
const char *mclw_hal_usb_serial(void);

#endif
