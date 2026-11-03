// SPDX-License-Identifier: Apache-2.0 OR MIT
// The engine's lifecycle: power, the output FIFO, the URC queue, the
// model ops and the property parts.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/uart.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"

#include "modem.h"

enum {
    PROP_POWERED,
    PROP_BOOT_MS,
    PROP_BAUD,
    PROP_ECHO,
    PROP_FUNCTIONALITY,
    PROP_SIM,
    PROP_REGISTRATION,
    PROP_RSSI,
    PROP_TIME,
    PROP_TIMEZONE,
    PROP_BATTERY_MV,
    PROP_BATTERY_PERCENT,
    PROP_COMMANDS,
    PROP_UNKNOWN_COMMANDS,
    PROP_POWER_CYCLES,
    PROP_COUNT,
};

_Static_assert(PROP_COUNT == MODEM_PROPERTY_COUNT, "MODEM_PROPERTIES and the modem part disagree");

void modem_trace(struct modem *m, enum mclw_trace_kind kind, uint32_t addr, uint16_t len, int16_t result) {
    mclw_trace(m->inst, kind, addr, len, result);
}

uint64_t modem_now(void) {
    return mclw_now_us();
}

static bool multiplexed(const struct modem *m) {
    return m->link != NULL && m->link->cmux;
}

size_t modem_out_free(const struct modem *m) {
    if (multiplexed(m)) {
        return modem_link_out_free(m);
    }
    return MODEM_OUT_SIZE - m->out_len;
}

void modem_fifo_out(struct modem *m, const void *data, size_t len) {
    const uint8_t *p = data;

    if (len > (size_t)MODEM_OUT_SIZE - m->out_len) {
        modem_trace(m, MCLW_TRACE_ERROR, 0, (uint16_t)len, -ENOSPC);
        return;
    }
    for (size_t i = 0; i < len; i++) {
        m->out[(m->out_head + m->out_len) % MODEM_OUT_SIZE] = p[i];
        m->out_len++;
    }
}

void modem_out(struct modem *m, const void *data, size_t len) {
    if (multiplexed(m)) {
        modem_link_out(m, data, len);
    } else {
        modem_fifo_out(m, data, len);
    }
}

void modem_out_str(struct modem *m, const char *str) {
    modem_out(m, str, strlen(str));
}

void modem_out_text(struct modem *m, const struct text *t) {
    modem_out(m, t->buf, t->len);
}

void modem_line(struct modem *m, const char *str) {
    struct text t;

    text_init(&t);
    text_str(&t, "\r\n");
    text_str(&t, str);
    text_str(&t, "\r\n");
    modem_out_text(m, &t);
}

// The HAL ring takes what it has room for; the rest waits for the next
// on_rx or time_advanced.
void modem_pump(struct modem *m) {
    if (m->link != NULL) {
        modem_link_flush(m);
    }
    while (m->out_len > 0) {
        size_t chunk = MODEM_OUT_SIZE - m->out_head;
        if (chunk > m->out_len) {
            chunk = m->out_len;
        }
        size_t sent = mclw_hal_uart_send(m->uart, &m->out[m->out_head], chunk);
        m->tx_queued += (uint32_t)sent;
        m->out_head = (uint16_t)((m->out_head + sent) % MODEM_OUT_SIZE);
        m->out_len = (uint16_t)(m->out_len - sent);
        if (sent < chunk) {
            break;
        }
    }
    if (m->out_len == 0) {
        m->out_head = 0;
    }
}

static bool per_arg(uint8_t kind) {
    return (kind & MODEM_EV_PER_ARG) != 0;
}

void modem_event(struct modem *m, uint32_t delay_us, uint8_t kind, uint8_t arg) {
    uint32_t due = (uint32_t)modem_now() + delay_us;
    struct modem_event *slot = NULL;

    for (size_t i = 0; i < m->event_count; i++) {
        struct modem_event *e = &m->events[i];
        if (e->kind == kind && (!per_arg(kind) || e->arg == arg)) {
            if (kind == MODEM_EV_REG) {
                e->arg = (uint8_t)((e->arg & 0xf0u) | arg);
            } else if (!per_arg(kind)) {
                e->arg = arg;
                e->due_us = due;
                e->seq = m->event_seq++;
            }
            return;
        }
        if (e->kind == MODEM_EV_NONE && slot == NULL) {
            slot = e;
        }
    }
    if (slot == NULL) {
        modem_trace(m, MCLW_TRACE_ERROR, kind, arg, -ENOSPC);
        return;
    }
    slot->due_us = due;
    slot->seq = m->event_seq++;
    slot->kind = kind;
    slot->arg = arg;
}

void modem_event_cancel(struct modem *m, uint8_t kind, uint8_t arg) {
    for (size_t i = 0; i < m->event_count; i++) {
        struct modem_event *e = &m->events[i];
        if (e->kind == kind && e->arg == arg) {
            e->kind = MODEM_EV_NONE;
        }
    }
}

struct modem_chan *modem_chan_at(struct modem *m, size_t i) {
    if (i == 0) {
        return &m->ses.raw;
    }
    if (m->link == NULL || i > MODEM_LINK_CHANNELS) {
        return NULL;
    }
    return &m->link->chans[i - 1];
}

// No command line half received, no data phase open and no final result
// pending on any channel that takes commands, and at least one does.
static bool idle(struct modem *m) {
    bool any = false;

    if (m->power != MODEM_ON || m->ses.pending_baud != 0) {
        return false;
    }
    for (size_t i = 0;; i++) {
        const struct modem_chan *ch = modem_chan_at(m, i);
        if (ch == NULL) {
            break;
        }
        if (ch->mode != MODEM_CHAN_COMMAND) {
            continue;
        }
        if (ch->line_len != 0 || ch->phase != NULL || ch->final_pending) {
            return false;
        }
        any = true;
    }
    return any;
}

// URCs go to every channel in command mode at once.
static void select_urc(struct modem *m) {
    modem_select(m, &m->ses.raw);
    if (m->link != NULL && m->link->cmux) {
        modem_link_flush(m);
        m->link->out_mask = modem_link_urc_mask(m);
        for (size_t i = 0; modem_chan_at(m, i) != NULL; i++) {
            if (modem_chan_at(m, i)->mode == MODEM_CHAN_COMMAND) {
                m->chan = modem_chan_at(m, i);
                break;
            }
        }
    }
}

static bool event_before(const struct modem_event *a, const struct modem_event *b) {
    if (a->due_us != b->due_us) {
        return (int32_t)(a->due_us - b->due_us) < 0;
    }
    return (int16_t)(a->seq - b->seq) < 0;
}

static void boot_complete(struct modem *m);

// The end of boot and a deferred final result never wait. URCs then go in
// time order, each once no command is in flight, no data phase is open and
// the FIFO has room for it.
static void deliver(struct modem *m, uint64_t now_us) {
    if (m->power == MODEM_BOOTING && now_us >= m->boot_due_us) {
        boot_complete(m);
    }
    for (size_t i = 0; m->power == MODEM_ON; i++) {
        struct modem_chan *ch = modem_chan_at(m, i);
        if (ch == NULL) {
            break;
        }
        if (ch->final_pending && now_us >= ch->final_due_us) {
            modem_select(m, ch);
            ch->final_pending = false;
            modem_ok(m);
            modem_at_resume(m);
        }
    }
    for (size_t guard = 0; guard < 2u * m->event_count; guard++) {
        struct modem_event *best = NULL;

        if (!idle(m)) {
            return;
        }
        select_urc(m);
        if (modem_out_free(m) < m->config->urc_room) {
            return;
        }
        for (size_t i = 0; i < m->event_count; i++) {
            struct modem_event *e = &m->events[i];
            if (e->kind == MODEM_EV_NONE || (int32_t)((uint32_t)now_us - e->due_us) < 0) {
                continue;
            }
            if (best == NULL || event_before(e, best)) {
                best = e;
            }
        }
        if (best == NULL) {
            return;
        }
        uint8_t kind = best->kind;
        uint8_t arg = best->arg;
        best->kind = MODEM_EV_NONE;
        if (kind == MODEM_EV_REG || kind == MODEM_EV_CPIN) {
            modem_standard_event(m, kind, arg);
        } else {
            m->config->event(m, kind, arg);
        }
        if (m->link != NULL) {
            modem_link_flush(m);
        }
    }
}

static void clear_events(struct modem *m) {
    memset(m->events, 0, m->event_count * sizeof(m->events[0]));
}

static void session_defaults(struct modem *m) {
    const struct modem_config *c = m->config;

    memset(&m->ses, 0, sizeof(m->ses));
    m->ses.raw.echo = true;
    m->ses.cfun = 1;
    m->chan = &m->ses.raw;
    if (m->link != NULL) {
        modem_link_session(m);
    }
    for (size_t i = 0; i < c->part_count; i++) {
        if (c->parts[i]->session != NULL) {
            c->parts[i]->session(m);
        }
    }
}

void modem_power_on(struct modem *m) {
    clear_events(m);
    session_defaults(m);
    m->power = MODEM_BOOTING;
    m->boot_due_us = modem_now() + (uint64_t)m->set.boot_ms * 1000u;
    modem_trace(m, MCLW_TRACE_STATE, MODEM_BOOTING, 0, 0);
}

void modem_power_off(struct modem *m, bool announce) {
    if (announce) {
        if (m->config->power_down != NULL) {
            modem_line(m, m->config->power_down);
        }
    } else {
        m->out_head = 0;
        m->out_len = 0;
    }
    clear_events(m);
    session_defaults(m);
    m->power = MODEM_OFF;
    modem_trace(m, MCLW_TRACE_STATE, MODEM_OFF, 0, 0);
}

static void boot_complete(struct modem *m) {
    m->power = MODEM_ON;
    session_defaults(m);
    m->ses.reported_stat = modem_stat(m);
    m->ses.reported_ps_stat = modem_ps_stat(m);
    m->config->booted(m);
    m->stats.power_cycles++;
    modem_trace(m, MCLW_TRACE_STATE, MODEM_ON, 0, 0);
}

static void apply_baud(struct modem *m, uint32_t baud) {
    if (mclw_hal_uart_set_baud(m->uart, baud) == 0) {
        m->set.baud = baud;
    }
}

// The IPR answer leaves at the old rate: the switch waits until the FIFO
// is empty, and the engine's set_baud drains its own ring first.
static void finish_output(struct modem *m) {
    if (m->link != NULL) {
        modem_link_service(m);
    }
    modem_pump(m);
    if (m->ses.pending_baud != 0 && m->out_len == 0) {
        apply_baud(m, m->ses.pending_baud);
        m->ses.pending_baud = 0;
    }
}

// Every press does one thing, and the key must be released before the
// next one counts. A key already down at attach or reset counts as used,
// so a DUT pin floating low before its driver runs is not a press.
static void sample_pwrkey(struct modem *m, uint64_t now_us) {
    bool pressed = !mclw_hal_gpio_read(m->pwrkey);

    if (!pressed) {
        m->key_down = false;
        m->key_used = false;
        return;
    }
    if (!m->key_down) {
        m->key_down = true;
        m->key_since_us = now_us;
    }
    if (m->key_used) {
        return;
    }
    uint64_t held = now_us - m->key_since_us;
    if (m->power == MODEM_OFF && held >= m->config->pwrkey_on_us) {
        m->key_used = true;
        modem_power_on(m);
    } else if (m->power != MODEM_OFF && held >= m->config->pwrkey_off_us) {
        m->key_used = true;
        modem_power_off(m, true);
    }
}

// RESET_N: low for `reset_us` holds the modem in reset, losing what it had
// not sent, and the release boots it. A line already low at attach or
// reset is not a reset until it has been high.
static void sample_reset(struct modem *m, uint64_t now_us) {
    bool low = !mclw_hal_gpio_read(m->pwrkey);

    if (!low) {
        if (m->key_down && !m->key_used && m->power == MODEM_OFF) {
            modem_power_on(m);
        }
        m->key_down = false;
        m->key_used = false;
        return;
    }
    if (!m->key_down) {
        m->key_down = true;
        m->key_since_us = now_us;
    }
    if (!m->key_used && m->power != MODEM_OFF && now_us - m->key_since_us >= m->config->reset_us) {
        modem_power_off(m, false);
    }
}

static uint32_t default_baud(const struct modem *m) {
    return m->inst->model->bindings[MODEM_BINDING_UART].uart.default_baud;
}

static void defaults(struct modem *m) {
    const struct modem_config *c = m->config;
    struct modem_settings *set = &m->set;

    memset(set, 0, sizeof(*set));
    set->boot_ms = c->boot_ms;
    set->baud = default_baud(m);
    set->registration = MODEM_REG_HOME;
    set->rssi = MODEM_DEFAULT_RSSI;
    set->time[0] = MODEM_DEFAULT_YEAR;
    set->time[1] = 1;
    set->time[2] = 1;
    set->battery_mv = MODEM_DEFAULT_BATTERY_MV;
    set->battery_percent = MODEM_DEFAULT_BATTERY_PERCENT;
    for (size_t i = 0; i < c->part_count; i++) {
        if (c->parts[i]->defaults != NULL) {
            c->parts[i]->defaults(m);
        }
    }
}

static void clear_stats(struct modem *m) {
    const struct modem_config *c = m->config;

    memset(&m->stats, 0, sizeof(m->stats));
    for (size_t i = 0; i < c->part_count; i++) {
        if (c->parts[i]->clear != NULL) {
            c->parts[i]->clear(m);
        }
    }
}

// The attach state: off until PWRKEY is pressed when the key is wired,
// otherwise booting at once.
static void start(struct modem *m) {
    m->out_head = 0;
    m->out_len = 0;
    clear_events(m);
    session_defaults(m);
    m->power = MODEM_OFF;
    m->key_down = m->pwrkey != NULL && !mclw_hal_gpio_read(m->pwrkey);
    m->key_used = m->key_down;
    m->key_since_us = modem_now();
    if (m->pwrkey == NULL || m->config->power_pin == MODEM_PIN_RESET) {
        modem_power_on(m);
    }
}

static void on_rx(struct mclw_uart *uart, const uint8_t *bytes, size_t len, void *ctx) {
    struct modem *m = ctx;

    (void)uart;
    if (m->link != NULL) {
        modem_link_rx(m, bytes, len);
    } else {
        modem_at_rx(m, bytes, len);
    }
    deliver(m, modem_now());
    finish_output(m);
}

int modem_init(struct modem *m, struct mclw_instance *inst, const struct mclw_binding *bindings,
    struct modem_event *events, uint8_t event_count) {
    const struct mclw_binding *key = &bindings[MODEM_BINDING_PWRKEY];

    m->inst = inst;
    m->config = inst->model->config;
    m->uart = bindings[MODEM_BINDING_UART].uart;
    m->pwrkey = key->present ? key->gpio : NULL;
    m->events = events;
    m->event_count = event_count;
    m->chan = &m->ses.raw;
    if (m->link != NULL) {
        modem_link_init(m);
    }
    defaults(m);
    start(m);
    mclw_hal_uart_set_rx(m->uart, on_rx, m);
    return 0;
}

void modem_deinit(struct mclw_instance *inst) {
    struct modem *m = inst->state;

    mclw_hal_uart_set_rx(m->uart, NULL, NULL);
    m->uart = NULL;
    m->pwrkey = NULL;
}

int modem_reset(struct mclw_instance *inst, bool factory) {
    struct modem *m = inst->state;

    clear_stats(m);
    if (factory) {
        defaults(m);
        apply_baud(m, default_baud(m));
    }
    start(m);
    return 0;
}

// The part a property or region index falls in, and the index within it.
static const struct modem_part *part_at(const struct modem *m, size_t *index, bool region) {
    const struct modem_config *c = m->config;

    for (size_t i = 0; i < c->part_count; i++) {
        const struct modem_part *p = c->parts[i];
        size_t count = region ? p->region_count : p->property_count;
        if (*index < count) {
            return p;
        }
        *index -= count;
    }
    return NULL;
}

int modem_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct modem *m = inst->state;
    const struct modem_part *p = part_at(m, &property, false);

    if (p == NULL || p->get == NULL) {
        return -EINVAL;
    }
    return p->get(m, property, out);
}

int modem_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct modem *m = inst->state;
    const struct modem_part *p = part_at(m, &property, false);

    if (p == NULL || p->set == NULL) {
        return -EINVAL;
    }
    return p->set(m, property, in);
}

int modem_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    struct modem *m = inst->state;
    const struct modem_part *p = part_at(m, &region, true);
    uint8_t *bytes = p != NULL ? p->region(m, region) : NULL;

    if (bytes == NULL) {
        return -EINVAL;
    }
    memcpy(buf, bytes + offset, len);
    return 0;
}

int modem_write_region(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf,
    size_t len) {
    struct modem *m = inst->state;
    const struct modem_part *p = part_at(m, &region, true);
    uint8_t *bytes = p != NULL ? p->region(m, region) : NULL;

    if (bytes == NULL) {
        return -EINVAL;
    }
    memcpy(bytes + offset, buf, len);
    if (p->written != NULL) {
        p->written(m, region);
    }
    return 0;
}

void modem_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct modem *m = inst->state;

    if (m->pwrkey != NULL && m->config->power_pin == MODEM_PIN_RESET) {
        sample_reset(m, now_us);
    } else if (m->pwrkey != NULL) {
        sample_pwrkey(m, now_us);
    }
    modem_at_tick(m, now_us);
    if (m->link != NULL) {
        modem_link_tick(m, now_us);
    }
    deliver(m, now_us);
    finish_output(m);
    for (size_t i = 0; modem_chan_at(m, i) != NULL; i++) {
        modem_select(m, modem_chan_at(m, i));
        modem_at_resume(m);
    }
    deliver(m, now_us);
    finish_output(m);
}

static int modem_part_get(struct modem *m, size_t property, struct mclw_value *out) {
    const struct modem_settings *set = &m->set;

    switch (property) {
        case PROP_POWERED:
            out->v[0] = m->power != MODEM_OFF;
            return 0;
        case PROP_BOOT_MS:
            out->v[0] = set->boot_ms;
            return 0;
        case PROP_BAUD:
            out->v[0] = set->baud;
            return 0;
        case PROP_ECHO:
            out->v[0] = m->power == MODEM_ON && m->ses.raw.echo;
            return 0;
        case PROP_FUNCTIONALITY:
            out->v[0] = m->power == MODEM_ON ? m->ses.cfun : 0;
            return 0;
        case PROP_SIM:
            out->v[0] = set->sim;
            return 0;
        case PROP_REGISTRATION:
            out->v[0] = set->registration;
            return 0;
        case PROP_RSSI:
            out->v[0] = set->rssi;
            return 0;
        case PROP_TIME:
            for (size_t i = 0; i < 6; i++) {
                out->v[i] = set->time[i];
            }
            return 0;
        case PROP_TIMEZONE:
            out->v[0] = set->timezone;
            return 0;
        case PROP_BATTERY_MV:
            out->v[0] = set->battery_mv;
            return 0;
        case PROP_BATTERY_PERCENT:
            out->v[0] = set->battery_percent;
            return 0;
        case PROP_COMMANDS:
            out->v[0] = m->stats.commands;
            return 0;
        case PROP_UNKNOWN_COMMANDS:
            out->v[0] = m->stats.unknown_commands;
            return 0;
        case PROP_POWER_CYCLES:
            out->v[0] = m->stats.power_cycles;
            return 0;
        default:
            return -EINVAL;
    }
}

static int modem_part_set(struct modem *m, size_t property, const struct mclw_value *in) {
    struct modem_settings *set = &m->set;
    int64_t v = in->v[0];

    switch (property) {
        case PROP_POWERED:
            if (v != 0 && m->power == MODEM_OFF) {
                modem_power_on(m);
            } else if (v == 0 && m->power != MODEM_OFF) {
                modem_power_off(m, false);
            }
            return 0;
        case PROP_BOOT_MS:
            set->boot_ms = (uint32_t)v;
            return 0;
        case PROP_BAUD:
            apply_baud(m, (uint32_t)v);
            return set->baud == (uint32_t)v ? 0 : -EIO;
        case PROP_SIM:
            if (set->sim != (uint8_t)v) {
                set->sim = (uint8_t)v;
                m->ses.pin_entered = false;
                if (m->power == MODEM_ON) {
                    modem_event(m, 0, MODEM_EV_CPIN, modem_sim_cpin(m));
                }
            }
            modem_network_changed(m);
            return 0;
        case PROP_REGISTRATION:
            set->registration = (uint8_t)v;
            modem_network_changed(m);
            return 0;
        case PROP_RSSI:
            if (v > MODEM_RSSI_MAX && v != MODEM_RSSI_UNKNOWN) {
                return -EINVAL;
            }
            set->rssi = (uint8_t)v;
            return 0;
        case PROP_TIME: {
            int32_t time[6];
            for (size_t i = 0; i < 6; i++) {
                time[i] = (int32_t)in->v[i];
            }
            if (!modem_time_valid(time)) {
                return -EINVAL;
            }
            memcpy(set->time, time, sizeof(time));
            return 0;
        }
        case PROP_TIMEZONE:
            set->timezone = (int8_t)v;
            return 0;
        case PROP_BATTERY_MV:
            set->battery_mv = (uint16_t)v;
            return 0;
        case PROP_BATTERY_PERCENT:
            set->battery_percent = (uint8_t)v;
            return 0;
        default:
            return -EINVAL;
    }
}

// The engine resets its own state itself; the part only maps properties.
const struct modem_part modem_engine_part = {
    .property_count = MODEM_PROPERTY_COUNT,
    .region_count = 0,
    .defaults = NULL,
    .clear = NULL,
    .session = NULL,
    .get = modem_part_get,
    .set = modem_part_set,
    .region = NULL,
    .written = NULL,
};
