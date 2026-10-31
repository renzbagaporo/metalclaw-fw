// SPDX-License-Identifier: Apache-2.0 OR MIT
// The USB device is brought up once, before the first mp_init, and is never
// torn down; a soft reset changes nothing here.

#include "py/runtime.h"
#include "py/ringbuf.h"
#include "py/mphal.h"
#include "shared/runtime/interrupt_char.h"
#include "mphalport.h"
#include "pico/time.h"
#include "tusb.h"
#include "metalclaw/hal/console.h"
#include "metalclaw_port.h"
#include "usb_internal.h"

#define CDC_ITF 0

// Changes to the interface set are collected for this long, then applied
// with one disconnect, and only if the descriptor set really changed: a
// soft reset that re-attaches the same devices never re-enumerates. The
// host must see the pull-up gone for a while or it keeps the old
// configuration, hence the gap before reconnecting.
//
// Re-enumeration only drops and restores the pull-up; the host's bus reset
// on reconnect resets the class drivers and endpoints, and the next
// SET_CONFIGURATION opens the new set. TinyUSB stays initialised: on this
// port tud_init claims two of the eight free hardware spinlocks and
// tud_deinit releases one, so a deinit/init cycle would leak a spinlock
// per re-enumeration until critical_section_init panics.
#define REENUMERATE_DEBOUNCE_US 200000
#define REENUMERATE_GAP_US      100000

// Owned by the scheduler once linked; mp_init keeps pending static nodes
// across a soft reset, so this file must never touch its fields.
static mp_sched_node_t usb_task_node;

static enum { REENUM_IDLE, REENUM_PENDING, REENUM_DISCONNECTED } reenum_state;
static uint64_t reenum_due_us;
static alarm_id_t reenum_alarm;

static void usb_task_callback(mp_sched_node_t *node);

// With the device off the bus there are no USB interrupts to wake an idle
// VM, so each phase is driven by an alarm that schedules the USB task.
static int64_t reenum_alarm_cb(alarm_id_t id, void *user) {
    (void)id;
    (void)user;
    reenum_alarm = 0;
    mp_sched_schedule_node(&usb_task_node, usb_task_callback);
    return 0;
}

static void reenum_schedule(uint64_t delay_us) {
    reenum_due_us = time_us_64() + delay_us;
    if (reenum_alarm > 0) {
        cancel_alarm(reenum_alarm);
    }
    reenum_alarm = add_alarm_in_us(delay_us + 1000, reenum_alarm_cb, NULL, true);
}

void mclw_usb_init(void) {
    if (!tud_inited()) {
        mclw_usb_descriptors_rebuild();
        tud_init(0);
    }
}

void mclw_usb_request_reenumeration(void) {
    if (reenum_state != REENUM_DISCONNECTED) {
        reenum_state = REENUM_PENDING;
        reenum_schedule(REENUMERATE_DEBOUNCE_US);
    }
}

static void reenumerate_step(void) {
    if (reenum_state == REENUM_IDLE || time_us_64() < reenum_due_us) {
        return;
    }
    if (reenum_state == REENUM_PENDING) {
        if (!mclw_usb_descriptors_rebuild()) {
            reenum_state = REENUM_IDLE;
            return;
        }
        tud_disconnect();
        reenum_state = REENUM_DISCONNECTED;
        reenum_schedule(REENUMERATE_GAP_US);
        return;
    }
    tud_connect();
    reenum_state = REENUM_IDLE;
}

void mclw_usb_task(void) {
    reenumerate_step();
    if (tud_inited()) {
        tud_task_ext(0, false);
    }
}

static void usb_task_callback(mp_sched_node_t *node) {
    (void)node;
    mclw_usb_task();
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

size_t mclw_hal_console_write(const void *buf, size_t size) {
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
            mclw_usb_task();
        }
    }
    return written;
}

size_t mclw_hal_console_read(void *buf, size_t size) {
    return tud_cdc_n_read(CDC_ITF, buf, size);
}

bool mclw_hal_console_readable(void) {
    return tud_cdc_n_available(CDC_ITF) > 0;
}

bool mclw_hal_console_writable(void) {
    return tud_cdc_n_write_available(CDC_ITF) > 0;
}
