// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_USB_INTERNAL_H
#define MCLW_USB_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Open vendor interfaces in descriptor order; `names[i]` is the interface
// string of vendor interface i.
size_t mclw_usb_vendor_layout(const char **names, size_t max);

// Rebuilds the configuration from the open interfaces; true if the result
// differs from what the host currently has.
bool mclw_usb_descriptors_rebuild(void);
void mclw_usb_request_reenumeration(void);

#endif
