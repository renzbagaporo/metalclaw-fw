// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <string.h>

#include "hardware/sync.h"
#include "pico/time.h"
#include "tusb.h"
#include "metalclaw/hal/usb.h"
#include "usb_internal.h"

// What a serial function's device sends waits here for the host: 4 KiB is
// about 350 ms of a UART at 115200 baud.
#define SERIAL_QUEUE_SIZE 4096u
#define SERIAL_QUEUE_MASK (SERIAL_QUEUE_SIZE - 1u)
#define SERIAL_STAGE_SIZE CFG_TUD_CDC_EP_BUFSIZE
// Neither data the worker queued nor bytes a model left unconsumed raise a
// USB event, so while a serial function is open core 0 looks this often.
#define SERIAL_TICK_US 1000

struct serial_state {
    // Free-running. Producers are model contexts, serialised by the runtime
    // lock; the USB task is the only consumer.
    volatile uint32_t head;
    volatile uint32_t tail;
    uint8_t queue[SERIAL_QUEUE_SIZE];
    // Host bytes read from TinyUSB that the model has not taken yet.
    uint8_t stage[SERIAL_STAGE_SIZE];
    volatile uint16_t stage_len;
    volatile uint16_t stage_at;
    volatile uint8_t lines;
    cdc_line_coding_t coding;
};

struct mclw_usb_interface {
    bool open;
    // In the descriptor set the host has: `index` is then the function's
    // position among the open ones of its kind, which is how TinyUSB knows it.
    bool live;
    uint8_t index;
    const struct mclw_usb_interface_desc *desc;
    mclw_usb_rx_t rx;
    mclw_usb_control_t control;
    void *ctx;
    const void *owner;
    struct serial_state *serial;
};

static struct mclw_usb_interface vendor_slots[CFG_TUD_VENDOR];
static struct mclw_usb_interface serial_slots[MCLW_USB_SERIAL_MAX];
static struct serial_state serial_states[MCLW_USB_SERIAL_MAX];
static repeating_timer_t serial_timer;
static bool serial_timer_running;

static void layout(struct mclw_usb_layout *out, bool take) {
    out->serial_count = 0;
    out->vendor_count = 0;
    for (size_t i = 0; i < MCLW_USB_SERIAL_MAX; i++) {
        struct mclw_usb_interface *s = &serial_slots[i];
        if (s->open) {
            if (take) {
                s->index = (uint8_t)out->serial_count;
                s->live = true;
            }
            out->serial_names[out->serial_count++] = s->desc->name;
        }
    }
    for (size_t i = 0; i < CFG_TUD_VENDOR; i++) {
        struct mclw_usb_interface *s = &vendor_slots[i];
        if (s->open) {
            if (take) {
                s->index = (uint8_t)out->vendor_count;
                s->live = true;
            }
            out->vendor_names[out->vendor_count++] = s->desc->name;
        }
    }
}

void mclw_usb_layout_peek(struct mclw_usb_layout *out) {
    layout(out, false);
}

void mclw_usb_layout_take(struct mclw_usb_layout *out) {
    layout(out, true);
}

static bool serial_pending(void) {
    for (size_t i = 0; i < MCLW_USB_SERIAL_MAX; i++) {
        const struct serial_state *st = &serial_states[i];
        if (serial_slots[i].open && (st->head != st->tail || st->stage_at != st->stage_len)) {
            return true;
        }
    }
    return false;
}

static bool serial_tick(repeating_timer_t *timer) {
    (void)timer;
    if (serial_pending()) {
        mclw_usb_schedule();
    }
    return true;
}

static bool serial_any_open(void) {
    for (size_t i = 0; i < MCLW_USB_SERIAL_MAX; i++) {
        if (serial_slots[i].open) {
            return true;
        }
    }
    return false;
}

struct mclw_usb_interface *mclw_hal_usb_open(const struct mclw_usb_interface_desc *desc,
    mclw_usb_rx_t rx, mclw_usb_control_t control, void *ctx, const void *owner, int *err) {
    if (desc == NULL || desc->name == NULL || rx == NULL || owner == NULL ||
        (desc->function != MCLW_USB_VENDOR && desc->function != MCLW_USB_SERIAL)) {
        *err = -EINVAL;
        return NULL;
    }
    bool serial = desc->function == MCLW_USB_SERIAL;
    struct mclw_usb_interface *slots = serial ? serial_slots : vendor_slots;
    size_t count = serial ? MCLW_USB_SERIAL_MAX : CFG_TUD_VENDOR;
    struct mclw_usb_interface *s = NULL;
    for (size_t i = 0; i < count; i++) {
        if (!slots[i].open) {
            s = &slots[i];
            break;
        }
    }
    if (s == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    if (serial && !serial_timer_running) {
        if (!add_repeating_timer_us(-SERIAL_TICK_US, serial_tick, NULL, &serial_timer)) {
            *err = -ENOMEM;
            return NULL;
        }
        serial_timer_running = true;
    }
    memset(s, 0, sizeof(*s));
    s->desc = desc;
    s->rx = rx;
    s->control = control;
    s->ctx = ctx;
    s->owner = owner;
    if (serial) {
        struct serial_state *st = &serial_states[s - serial_slots];
        st->head = 0;
        st->tail = 0;
        st->stage_len = 0;
        st->stage_at = 0;
        st->lines = 0;
        st->coding.bit_rate = 115200;
        st->coding.stop_bits = 0;
        st->coding.parity = 0;
        st->coding.data_bits = 8;
        s->serial = st;
    }
    s->open = true;
    // Re-attaching what the host already has, as main.py does after a soft
    // reset, is served at once; anything else waits for the new set.
    struct mclw_usb_layout prospective;
    mclw_usb_layout_peek(&prospective);
    if (mclw_usb_descriptors_match(&prospective)) {
        mclw_usb_layout_take(&prospective);
    }
    mclw_usb_request_reenumeration();
    *err = 0;
    return s;
}

void mclw_hal_usb_close(struct mclw_usb_interface *s) {
    if (s == NULL || !s->open) {
        return;
    }
    memset(s, 0, sizeof(*s));
    if (serial_timer_running && !serial_any_open()) {
        cancel_repeating_timer(&serial_timer);
        serial_timer_running = false;
    }
    mclw_usb_request_reenumeration();
}

static size_t serial_write(struct serial_state *st, const uint8_t *data, size_t len) {
    uint32_t head = st->head;
    uint32_t room = SERIAL_QUEUE_SIZE - (head - st->tail);
    size_t n = len < room ? len : room;
    size_t first = SERIAL_QUEUE_SIZE - (head & SERIAL_QUEUE_MASK);

    if (first > n) {
        first = n;
    }
    memcpy(&st->queue[head & SERIAL_QUEUE_MASK], data, first);
    memcpy(st->queue, data + first, n - first);
    __dmb();
    st->head = head + (uint32_t)n;
    return n;
}

size_t mclw_hal_usb_write(struct mclw_usb_interface *s, const uint8_t *data, size_t len) {
    if (s == NULL || !s->open || data == NULL) {
        return 0;
    }
    if (s->serial != NULL) {
        return serial_write(s->serial, data, len);
    }
    if (!s->live || !tud_inited() || !tud_vendor_n_mounted(s->index)) {
        return 0;
    }
    uint32_t n = tud_vendor_n_write(s->index, data, (uint32_t)len);
    tud_vendor_n_write_flush(s->index);
    return n;
}

bool mclw_hal_usb_connected(const struct mclw_usb_interface *s) {
    if (s == NULL || !s->open) {
        return false;
    }
    if (s->serial != NULL) {
        return (s->serial->lines & MCLW_USB_LINE_DTR) != 0u;
    }
    return s->live && tud_inited() && tud_vendor_n_mounted(s->index);
}

int mclw_hal_usb_line_coding(const struct mclw_usb_interface *s, struct mclw_usb_line_coding *out) {
    if (s == NULL || !s->open || s->serial == NULL || out == NULL) {
        return -EINVAL;
    }
    cdc_line_coding_t coding = s->serial->coding;

    out->baud = coding.bit_rate;
    if (coding.data_bits < 5u || coding.data_bits > 8u) {
        return -ENOTSUP;
    }
    out->format.data_bits = coding.data_bits;
    switch (coding.stop_bits) {
        case CDC_LINE_CODING_STOP_BITS_1:
            out->format.stop_bits = 1;
            break;
        case CDC_LINE_CODING_STOP_BITS_2:
            out->format.stop_bits = 2;
            break;
        default:
            return -ENOTSUP;
    }
    switch (coding.parity) {
        case CDC_LINE_CODING_PARITY_NONE:
            out->format.parity = MCLW_UART_PARITY_NONE;
            break;
        case CDC_LINE_CODING_PARITY_ODD:
            out->format.parity = MCLW_UART_PARITY_ODD;
            break;
        case CDC_LINE_CODING_PARITY_EVEN:
            out->format.parity = MCLW_UART_PARITY_EVEN;
            break;
        default:
            return -ENOTSUP;
    }
    return 0;
}

uint8_t mclw_hal_usb_control_lines(const struct mclw_usb_interface *s) {
    if (s == NULL || !s->open || s->serial == NULL) {
        return 0;
    }
    return s->serial->lines;
}

static void serial_to_host(struct serial_state *st, uint8_t cdc) {
    uint32_t tail = st->tail;
    uint32_t head = st->head;

    __dmb();
    while (tail != head) {
        uint32_t space = tud_cdc_n_write_available(cdc);
        uint32_t chunk = head - tail;
        uint32_t contiguous = SERIAL_QUEUE_SIZE - (tail & SERIAL_QUEUE_MASK);

        if (space == 0u) {
            break;
        }
        if (chunk > contiguous) {
            chunk = contiguous;
        }
        if (chunk > space) {
            chunk = space;
        }
        uint32_t n = tud_cdc_n_write(cdc, &st->queue[tail & SERIAL_QUEUE_MASK], chunk);
        if (n == 0u) {
            break;
        }
        tail += n;
    }
    st->tail = tail;
    tud_cdc_n_write_flush(cdc);
}

// TinyUSB re-arms the OUT endpoint only while its FIFO has room for a
// packet, so bytes the model leaves in the stage hold the host off.
static void serial_from_host(struct mclw_usb_interface *s, uint8_t cdc) {
    struct serial_state *st = s->serial;

    for (;;) {
        if (st->stage_at == st->stage_len) {
            st->stage_at = 0;
            st->stage_len = (uint16_t)tud_cdc_n_read(cdc, st->stage, sizeof(st->stage));
            if (st->stage_len == 0u) {
                break;
            }
        }
        size_t left = (size_t)(st->stage_len - st->stage_at);
        size_t took = s->rx(s, &st->stage[st->stage_at], left, s->ctx);
        if (took > left) {
            took = left;
        }
        st->stage_at = (uint16_t)(st->stage_at + took);
        if (st->stage_at < st->stage_len) {
            break;
        }
    }
}

// TinyUSB forgets the control lines on a bus reset or a re-enumeration
// without a callback, so a function the host cannot reach reads them as low.
void mclw_usb_serial_pump(void) {
    bool up = tud_inited() && tud_mounted();

    for (size_t i = 0; i < MCLW_USB_SERIAL_MAX; i++) {
        struct mclw_usb_interface *s = &serial_slots[i];
        uint8_t cdc = (uint8_t)(1u + s->index);
        if (!s->open) {
            continue;
        }
        if (!up || !s->live || !tud_cdc_n_ready(cdc)) {
            s->serial->lines = 0;
            continue;
        }
        s->serial->lines = tud_cdc_n_get_line_state(cdc);
        tud_cdc_n_get_line_coding(cdc, &s->serial->coding);
        serial_to_host(s->serial, cdc);
        serial_from_host(s, cdc);
    }
}

static struct mclw_usb_interface *serial_for_cdc(uint8_t cdc) {
    for (size_t i = 0; cdc > 0u && i < MCLW_USB_SERIAL_MAX; i++) {
        struct mclw_usb_interface *s = &serial_slots[i];
        if (s->open && s->live && s->index == cdc - 1u) {
            return s;
        }
    }
    return NULL;
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    struct mclw_usb_interface *s = serial_for_cdc(itf);
    if (s == NULL) {
        return;
    }
    struct serial_state *st = s->serial;
    if (dtr && (st->lines & MCLW_USB_LINE_DTR) == 0u) {
        st->tail = st->head;
        tud_cdc_n_write_clear(itf);
    }
    st->lines = (uint8_t)((dtr ? MCLW_USB_LINE_DTR : 0u) | (rts ? MCLW_USB_LINE_RTS : 0u));
    if (s->control != NULL) {
        s->control(s, s->ctx);
    }
}

void tud_cdc_line_coding_cb(uint8_t itf, const cdc_line_coding_t *coding) {
    struct mclw_usb_interface *s = serial_for_cdc(itf);
    if (s == NULL) {
        return;
    }
    s->serial->coding = *coding;
    if (s->control != NULL) {
        s->control(s, s->ctx);
    }
}

// TinyUSB has already copied the packet into the interface's RX stream;
// drain it after handling or the FIFO fills and the OUT endpoint stops.
void tud_vendor_rx_cb(uint8_t itf, const uint8_t *buffer, uint16_t bufsize) {
    for (size_t i = 0; i < CFG_TUD_VENDOR; i++) {
        struct mclw_usb_interface *s = &vendor_slots[i];
        if (s->open && s->live && s->index == itf) {
            (void)s->rx(s, buffer, bufsize, s->ctx);
            break;
        }
    }
    tud_vendor_n_read_flush(itf);
}
