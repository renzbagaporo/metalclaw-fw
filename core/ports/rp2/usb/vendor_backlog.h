// SPDX-License-Identifier: Apache-2.0 OR MIT
// What a vendor function wrote that TinyUSB's TX FIFO had no room for, kept
// in order until the FIFO drains. Only the USB task touches it: writes from
// a function's rx callback and the drain from tud_vendor_tx_cb both run there.

#ifndef MCLW_USB_VENDOR_BACKLOG_H
#define MCLW_USB_VENDOR_BACKLOG_H

#include <stddef.h>
#include <stdint.h>

#define MCLW_VENDOR_BACKLOG_SIZE 512u

struct mclw_vendor_backlog {
    uint8_t buf[MCLW_VENDOR_BACKLOG_SIZE];
    uint16_t head;
    uint16_t count;
};

static inline void mclw_vendor_backlog_clear(struct mclw_vendor_backlog *b) {
    b->head = 0;
    b->count = 0;
}

// Appends as much of `data` as fits; returns how much that was.
static inline size_t mclw_vendor_backlog_push(struct mclw_vendor_backlog *b, const uint8_t *data, size_t len) {
    size_t room = MCLW_VENDOR_BACKLOG_SIZE - b->count;
    size_t n = len < room ? len : room;

    for (size_t i = 0; i < n; i++) {
        b->buf[(b->head + b->count + i) % MCLW_VENDOR_BACKLOG_SIZE] = data[i];
    }
    b->count = (uint16_t)(b->count + n);
    return n;
}

// The oldest bytes that sit contiguously, for one write.
static inline size_t mclw_vendor_backlog_peek(const struct mclw_vendor_backlog *b, const uint8_t **out) {
    size_t run = MCLW_VENDOR_BACKLOG_SIZE - b->head;

    *out = &b->buf[b->head];
    return b->count < run ? b->count : run;
}

static inline void mclw_vendor_backlog_drop(struct mclw_vendor_backlog *b, size_t n) {
    if (n > b->count) {
        n = b->count;
    }
    b->head = (uint16_t)((b->head + n) % MCLW_VENDOR_BACKLOG_SIZE);
    b->count = (uint16_t)(b->count - n);
}

#endif
