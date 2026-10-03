// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_USB_INTERNAL_H
#define MCLW_USB_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tusb.h"

// TinyUSB CDC instance 0 is the REPL; the rest are serial functions.
#define MCLW_USB_SERIAL_MAX (CFG_TUD_CDC - 1)

// The open functions in descriptor order, serial functions first; names
// are the interface strings. Taking the layout makes it the one the
// functions are served by: TinyUSB knows each by its position. Peeking
// changes nothing.
struct mclw_usb_layout {
    size_t serial_count;
    size_t vendor_count;
    const char *serial_names[MCLW_USB_SERIAL_MAX];
    const char *vendor_names[CFG_TUD_VENDOR];
};

void mclw_usb_layout_peek(struct mclw_usb_layout *out);
void mclw_usb_layout_take(struct mclw_usb_layout *out);

// Rebuilds the configuration from the open functions; true if the result
// differs from what the host currently has.
bool mclw_usb_descriptors_rebuild(void);
// Whether `layout` is the set the host has now.
bool mclw_usb_descriptors_match(const struct mclw_usb_layout *layout);
void mclw_usb_request_reenumeration(void);

// Schedules the USB task; from core 0, interrupt context included.
void mclw_usb_schedule(void);

// Moves serial data both ways; the USB task runs it after TinyUSB's.
void mclw_usb_serial_pump(void);

#endif
