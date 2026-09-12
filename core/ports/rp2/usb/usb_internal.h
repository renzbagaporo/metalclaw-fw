// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef METALCLAW_USB_INTERNAL_H
#define METALCLAW_USB_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Open vendor interfaces in descriptor order; `names[i]` is the interface
// string of vendor interface i.
size_t metalclaw_usb_vendor_layout(const char **names, size_t max);

// Rebuilds the configuration from the open interfaces; true if the result
// differs from what the host currently has.
bool metalclaw_usb_descriptors_rebuild(void);
void metalclaw_usb_request_reenumeration(void);

#endif
