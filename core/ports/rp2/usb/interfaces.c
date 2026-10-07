// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <string.h>

#include "tusb.h"
#include "metalclaw/hal/usb.h"
#include "usb_internal.h"

struct mclw_usb_interface {
    bool open;
    const struct mclw_usb_interface_desc *desc;
    mclw_usb_rx_t rx;
    void *ctx;
    const void *owner;
    // Vendor interface number TinyUSB knows this slot by, in the current
    // descriptor set. Only valid while the slot is open.
    uint8_t vendor_index;
};

static struct mclw_usb_interface slots[CFG_TUD_VENDOR];

static void assign_vendor_indices(void) {
    uint8_t n = 0;
    for (size_t i = 0; i < CFG_TUD_VENDOR; i++) {
        if (slots[i].open) {
            slots[i].vendor_index = n++;
        }
    }
}

size_t mclw_usb_vendor_layout(const char **names, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < CFG_TUD_VENDOR && n < max; i++) {
        if (slots[i].open) {
            names[n++] = slots[i].desc->name;
        }
    }
    return n;
}

struct mclw_usb_interface *mclw_hal_usb_open(const struct mclw_usb_interface_desc *desc,
    mclw_usb_rx_t rx, void *ctx, const void *owner, int *err) {
    if (desc == NULL || rx == NULL || owner == NULL) {
        *err = -EINVAL;
        return NULL;
    }
    struct mclw_usb_interface *s = NULL;
    for (size_t i = 0; i < CFG_TUD_VENDOR; i++) {
        if (!slots[i].open) {
            s = &slots[i];
            break;
        }
    }
    if (s == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    s->open = true;
    s->desc = desc;
    s->rx = rx;
    s->ctx = ctx;
    s->owner = owner;
    assign_vendor_indices();
    mclw_usb_request_reenumeration();
    *err = 0;
    return s;
}

void mclw_hal_usb_close(struct mclw_usb_interface *s) {
    if (s == NULL || !s->open) {
        return;
    }
    memset(s, 0, sizeof(*s));
    assign_vendor_indices();
    mclw_usb_request_reenumeration();
}

size_t mclw_hal_usb_write(struct mclw_usb_interface *s, const uint8_t *data, size_t len) {
    if (s == NULL || !s->open || !tud_inited() || !tud_vendor_n_mounted(s->vendor_index)) {
        return 0;
    }
    uint32_t n = tud_vendor_n_write(s->vendor_index, data, (uint32_t)len);
    tud_vendor_n_write_flush(s->vendor_index);
    return n;
}

bool mclw_hal_usb_connected(const struct mclw_usb_interface *s) {
    return s != NULL && s->open && tud_inited() && tud_vendor_n_mounted(s->vendor_index);
}

// TinyUSB has already copied the packet into the interface's RX stream;
// drain it after handling or the FIFO fills and the OUT endpoint stops.
void tud_vendor_rx_cb(uint8_t itf, const uint8_t *buffer, uint16_t bufsize) {
    for (size_t i = 0; i < CFG_TUD_VENDOR; i++) {
        struct mclw_usb_interface *s = &slots[i];
        if (s->open && s->vendor_index == itf) {
            s->rx(s, buffer, bufsize, s->ctx);
            break;
        }
    }
    tud_vendor_n_read_flush(itf);
}
