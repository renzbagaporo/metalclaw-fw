// SPDX-License-Identifier: Apache-2.0 OR MIT
// The USB device is brought up once, before the first mp_init, and is never
// torn down; a soft reset changes nothing here.

#include "py/runtime.h"
#include "py/ringbuf.h"
#include "py/mphal.h"
#include "shared/runtime/interrupt_char.h"
#include "mphalport.h"
#include "tusb.h"
#include "metalclaw_port.h"

#define CDC_ITF 0

// Owned by the scheduler once linked; mp_init keeps pending static nodes
// across a soft reset, so this file must never touch its fields.
static mp_sched_node_t usb_task_node;

void metalclaw_usb_init(void) {
    if (!tud_inited()) {
        tud_init(0);
    }
}

void metalclaw_usb_task(void) {
    if (tud_inited()) {
        tud_task_ext(0, false);
    }
}

static void usb_task_callback(mp_sched_node_t *node) {
    (void)node;
    metalclaw_usb_task();
}

void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)rhport;
    (void)eventid;
    (void)in_isr;
    mp_sched_schedule_node(&usb_task_node, usb_task_callback);
}

void tud_cdc_rx_cb(uint8_t itf) {
    if (itf != CDC_ITF) {
        return;
    }
    while (tud_cdc_n_available(CDC_ITF) && ringbuf_free(&stdin_ringbuf) > 0) {
        int32_t c = tud_cdc_n_read_char(CDC_ITF);
        if (c < 0) {
            break;
        }
        if (c == mp_interrupt_char) {
            stdin_ringbuf.iget = stdin_ringbuf.iput = 0;
            mp_sched_keyboard_interrupt();
        } else {
            ringbuf_put(&stdin_ringbuf, (uint8_t)c);
        }
    }
}

size_t metalclaw_console_write(const void *buf, size_t size) {
    const uint8_t *data = buf;
    size_t written = 0;
    mp_uint_t last_progress = mp_hal_ticks_ms();

    if (!tud_inited()) {
        return size;
    }
    while (written < size) {
        uint32_t n = size - written;
        if (tud_cdc_n_connected(CDC_ITF)) {
            n = MIN(n, tud_cdc_n_write_available(CDC_ITF));
        }
        uint32_t n2 = tud_cdc_n_write(CDC_ITF, data + written, n);
        tud_cdc_n_write_flush(CDC_ITF);
        written += n2;
        if (written < size) {
            if (n2 > 0) {
                last_progress = mp_hal_ticks_ms();
            } else if ((mp_uint_t)(mp_hal_ticks_ms() - last_progress) >= MICROPY_HW_USB_CDC_TX_TIMEOUT) {
                break;
            } else if (tud_cdc_n_connected(CDC_ITF)) {
                mp_event_wait_ms(1);
            }
            metalclaw_usb_task();
        }
    }
    return written;
}

size_t metalclaw_console_read(void *buf, size_t size) {
    return tud_cdc_n_read(CDC_ITF, buf, size);
}

bool metalclaw_console_readable(void) {
    return tud_cdc_n_available(CDC_ITF) > 0;
}

bool metalclaw_console_writable(void) {
    return tud_cdc_n_write_available(CDC_ITF) > 0;
}
