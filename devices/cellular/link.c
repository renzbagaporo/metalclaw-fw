// SPDX-License-Identifier: Apache-2.0 OR MIT
// The engine's link: the AT channels under CMUX, the multiplexer's sink,
// framing of channel output, the PPP transmit scheduler and its gate, the
// +CMUX and dial commands, and the link part.
//
// Output for a DLCI collects in one stage of N1 bytes and goes into the
// FIFO as UIH frames when it fills or the engine moves to another
// channel; a URC goes to every DLCI in command mode. Control replies and
// AT output go into the FIFO at once; PPP frames are added only while the
// UART bytes queued and in flight stay under MODEM_LINK_GATE, so an AT
// answer on DLCI 2 waits behind at most that much PPP.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/uart.h"
#include "metalclaw/value.h"

#include "cmux.h"
#include "ip.h"
#include "modem.h"
#include "net.h"
#include "ppp.h"
#include "text.h"

// 27.007 5.7: N1 when AT+CMUX leaves it out.
#define CMUX_DEFAULT_N1 31u
// The raw UART carries PPP in pieces of this much when there is no CMUX.
#define RAW_PPP_CHUNK 128u

enum {
    PROP_MRU,
    PROP_GW,
    PROP_DNS,
    PROP_CMUX,
    PROP_PPP_UP,
    PROP_LCP_TERMINATES,
    PROP_TCP_RETRANSMITS,
    PROP_FRAMES_BAD_FCS,
    PROP_DLCI_FC_STALLS,
    PROP_COUNT,
};

_Static_assert(PROP_COUNT == PPP_PROPERTY_COUNT, "PPP_PROPERTIES and the ppp part disagree");

static const uint8_t default_gw[4] = { 10, 0, 0, 1 };

static void settings_defaults(struct ppp_settings *set) {
    set->mru = PPP_MRU_DEFAULT;
    memcpy(set->gw, default_gw, 4);
    memcpy(set->dns, default_gw, 4);
}

void modem_select(struct modem *m, struct modem_chan *ch) {
    struct modem_link *l = m->link;

    m->chan = ch;
    if (l == NULL) {
        return;
    }
    uint8_t mask = l->cmux && ch->dlci != 0 ? (uint8_t)(1u << ch->dlci) : 0;
    if (mask != l->out_mask) {
        modem_link_flush(m);
        l->out_mask = mask;
    }
}

uint8_t modem_link_urc_mask(const struct modem *m) {
    const struct modem_link *l = m->link;
    uint8_t mask = 0;

    if (l == NULL || !l->cmux) {
        return 0;
    }
    if (m->ses.raw.mode == MODEM_CHAN_COMMAND && cmux_is_open(&l->mux, m->ses.raw.dlci)) {
        mask |= (uint8_t)(1u << m->ses.raw.dlci);
    }
    for (size_t i = 0; i < MODEM_LINK_CHANNELS; i++) {
        const struct modem_chan *ch = &l->chans[i];
        if (ch->mode == MODEM_CHAN_COMMAND && cmux_is_open(&l->mux, ch->dlci)) {
            mask |= (uint8_t)(1u << ch->dlci);
        }
    }
    return mask;
}

static void chan_reset(struct modem_chan *ch, uint8_t mode, uint8_t dlci, const struct modem_link *l) {
    memset(ch, 0, sizeof(*ch));
    ch->mode = mode;
    ch->dlci = dlci;
    ch->echo = l->echo;
    ch->numeric = l->numeric;
}

void modem_link_init(struct modem *m) {
    struct modem_link *l = m->link;

    ppp_init(&l->ppp, &l->set, &l->ip);
    ip_init(&l->ip, m->net);
    settings_defaults(&l->set);
}

static void bearer(struct modem *m, bool up) {
    if (m->config->link_bearer != NULL) {
        m->config->link_bearer(m, up);
    } else if (m->net != NULL) {
        m->net->ses.bearer = up;
    }
}

// The link's PPP stops without a word; its channel is left as it is.
static void ppp_stop(struct modem *m) {
    struct modem_link *l = m->link;
    bool was_up = l->ppp.ipcp_open;

    ppp_close(&l->ppp);
    l->ppp.events = 0;
    l->ppp_chan = NULL;
    if (was_up) {
        bearer(m, false);
    }
}

void modem_link_session(struct modem *m) {
    struct modem_link *l = m->link;

    // A power transition ends the bearer with the session; no hook runs.
    ppp_close(&l->ppp);
    l->ppp.events = 0;
    l->ppp_chan = NULL;
    l->cmux = false;
    l->stopped = false;
    l->out_mask = 0;
    l->stage_len = 0;
    l->echo = true;
    l->numeric = false;
    l->n1 = CMUX_DEFAULT_N1;
    cmux_start(&l->mux, l->n1);
    for (size_t i = 0; i < MODEM_LINK_CHANNELS; i++) {
        chan_reset(&l->chans[i], MODEM_CHAN_CLOSED, 0, l);
    }
}

void modem_link_flush(struct modem *m) {
    struct modem_link *l = m->link;
    uint8_t frame[CMUX_FRAME_MAX];

    if (l->stage_len == 0) {
        return;
    }
    for (uint8_t dlci = 1; dlci < 8; dlci++) {
        if ((l->out_mask & (1u << dlci)) != 0) {
            modem_fifo_out(m, frame, cmux_encode(frame, dlci, false, CMUX_UIH, l->stage, l->stage_len));
        }
    }
    l->stage_len = 0;
}

// The payload the FIFO still takes for every current DLCI, framing
// counted.
size_t modem_link_out_free(const struct modem *m) {
    const struct modem_link *l = m->link;
    size_t targets = 0;

    for (uint8_t dlci = 1; dlci < 8; dlci++) {
        targets += (l->out_mask >> dlci) & 1u;
    }
    if (targets == 0) {
        return 0;
    }
    size_t per = (MODEM_OUT_SIZE - m->out_len) / targets;
    size_t frame = (size_t)l->mux.n1 + CMUX_OVERHEAD;
    size_t rest = per % frame;
    size_t payload = (per / frame) * l->mux.n1 + (rest > CMUX_OVERHEAD ? rest - CMUX_OVERHEAD : 0u);
    return payload > l->stage_len ? payload - l->stage_len : 0;
}

void modem_link_out(struct modem *m, const void *data, size_t len) {
    struct modem_link *l = m->link;
    const uint8_t *p = data;

    // Under CMUX output that has no DLCI has nowhere to go.
    if (len > modem_link_out_free(m)) {
        modem_trace(m, MCLW_TRACE_ERROR, 0, (uint16_t)len, -ENOSPC);
        return;
    }
    while (len > 0) {
        size_t n = (size_t)l->mux.n1 - l->stage_len;
        if (n > len) {
            n = len;
        }
        memcpy(&l->stage[l->stage_len], p, n);
        l->stage_len = (uint8_t)(l->stage_len + n);
        p += n;
        len -= n;
        if (l->stage_len == l->mux.n1) {
            modem_link_flush(m);
        }
    }
}

static struct modem_chan *chan_of(struct modem *m, uint8_t dlci) {
    struct modem_link *l = m->link;

    if (dlci == 1) {
        return &m->ses.raw;
    }
    for (size_t i = 0; i < MODEM_LINK_CHANNELS; i++) {
        if (l->chans[i].mode != MODEM_CHAN_CLOSED && l->chans[i].dlci == dlci) {
            return &l->chans[i];
        }
    }
    return NULL;
}

static bool sink_open(void *ctx, uint8_t dlci) {
    struct modem *m = ctx;
    struct modem_link *l = m->link;
    struct modem_chan *ch = chan_of(m, dlci);

    for (size_t i = 0; i < MODEM_LINK_CHANNELS && ch == NULL; i++) {
        if (l->chans[i].mode == MODEM_CHAN_CLOSED) {
            ch = &l->chans[i];
        }
    }
    if (ch == NULL) {
        return false;
    }
    if (ch == l->ppp_chan) {
        ppp_stop(m);
    }
    chan_reset(ch, MODEM_CHAN_COMMAND, dlci, l);
    return true;
}

static void sink_close(void *ctx, uint8_t dlci) {
    struct modem *m = ctx;
    struct modem_link *l = m->link;
    struct modem_chan *ch = chan_of(m, dlci);

    if (ch == NULL) {
        return;
    }
    if (ch == l->ppp_chan) {
        ppp_stop(m);
    }
    chan_reset(ch, MODEM_CHAN_CLOSED, dlci, l);
    if (m->chan == ch) {
        modem_select(m, &m->ses.raw);
    }
}

static void sink_data(void *ctx, uint8_t dlci, const uint8_t *bytes, size_t len) {
    struct modem *m = ctx;
    struct modem_chan *ch = chan_of(m, dlci);

    if (ch != NULL && ch->mode != MODEM_CHAN_CLOSED) {
        modem_chan_rx(m, ch, bytes, len);
    }
}

static void sink_send(void *ctx, const uint8_t *frame, size_t len) {
    modem_fifo_out(ctx, frame, len);
}

// After the close-down the raw UART takes AT again at the same rate.
static void sink_closedown(void *ctx) {
    struct modem *m = ctx;
    struct modem_link *l = m->link;

    modem_link_flush(m);
    ppp_stop(m);
    l->cmux = false;
    l->out_mask = 0;
    for (size_t i = 0; i < MODEM_LINK_CHANNELS; i++) {
        chan_reset(&l->chans[i], MODEM_CHAN_CLOSED, 0, l);
    }
    chan_reset(&m->ses.raw, MODEM_CHAN_COMMAND, 0, l);
    m->chan = &m->ses.raw;
    modem_trace(m, MCLW_TRACE_STATE, modem_fourcc("CMUX"), 0, 0);
}

static const struct cmux_sink sink = {
    .open = sink_open,
    .close = sink_close,
    .data = sink_data,
    .send = sink_send,
    .closedown = sink_closedown,
};

void modem_link_rx(struct modem *m, const uint8_t *bytes, size_t len) {
    struct modem_link *l = m->link;

    while (len > 0 && m->power == MODEM_ON) {
        size_t n = l->cmux ? cmux_input(&l->mux, bytes, len, &sink, m) : modem_chan_rx(m, &m->ses.raw, bytes, len);
        if (n == 0) {
            return;
        }
        bytes += n;
        len -= n;
    }
}

// A link that came up raises the bearer; one that ended returns its
// channel to command mode with NO CARRIER, once the frame in progress is
// out.
static void link_events(struct modem *m) {
    struct modem_link *l = m->link;
    struct ppp *p = &l->ppp;

    if ((p->events & PPP_EV_UP) != 0) {
        p->events = (uint8_t)(p->events & ~PPP_EV_UP);
        if (p->ipcp_open) {
            bearer(m, true);
        }
        modem_trace(m, MCLW_TRACE_STATE, modem_fourcc("PPP+"), 0, 0);
    }
    if ((p->events & PPP_EV_DOWN) != 0 && !ppp_tx_busy(p)) {
        struct modem_chan *ch = l->ppp_chan;
        p->events = (uint8_t)(p->events & ~PPP_EV_DOWN);
        ppp_stop(m);
        bearer(m, false);
        if (ch != NULL && ch->mode == MODEM_CHAN_PPP) {
            ch->mode = MODEM_CHAN_COMMAND;
            modem_select(m, ch);
            modem_result(m, 3, "NO CARRIER");
        }
        modem_trace(m, MCLW_TRACE_STATE, modem_fourcc("PPP-"), 0, 0);
    }
}

static uint32_t in_flight(struct modem *m) {
    struct mclw_uart_stats st;

    mclw_hal_uart_stats(m->uart, &st);
    return m->out_len + (m->tx_queued - st.bytes_tx);
}

void modem_link_service(struct modem *m) {
    struct modem_link *l = m->link;
    uint8_t payload[CMUX_MTU];
    uint8_t frame[CMUX_FRAME_MAX];

    link_events(m);
    while (l->ppp_chan != NULL && in_flight(m) <= MODEM_LINK_GATE) {
        size_t n;
        if (l->cmux) {
            uint8_t dlci = l->ppp_chan->dlci;
            if (!cmux_can_send(&l->mux, dlci)) {
                if (!l->stopped) {
                    l->stopped = true;
                    l->stats.fc_stalls++;
                }
                break;
            }
            l->stopped = false;
            if ((size_t)MODEM_OUT_SIZE - m->out_len < CMUX_FRAME_MAX) {
                break;
            }
            n = ppp_pull(&l->ppp, payload, l->mux.n1, modem_now());
            if (n == 0) {
                break;
            }
            modem_fifo_out(m, frame, cmux_encode(frame, dlci, false, CMUX_UIH, payload, n));
        } else {
            if ((size_t)MODEM_OUT_SIZE - m->out_len < RAW_PPP_CHUNK) {
                break;
            }
            n = ppp_pull(&l->ppp, payload, RAW_PPP_CHUNK, modem_now());
            if (n == 0) {
                break;
            }
            modem_fifo_out(m, payload, n);
        }
    }
    link_events(m);
}

void modem_link_tick(struct modem *m, uint64_t now_us) {
    struct modem_link *l = m->link;

    if (l->ppp_chan != NULL) {
        ppp_tick(&l->ppp, now_us);
    }
}

void modem_cmd_cmux(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct modem_link *l = m->link;
    struct text t;
    int32_t v;
    int32_t n1 = CMUX_DEFAULT_N1;

    if (l == NULL) {
        modem_error(m);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_answer(m, "+CMUX: (0),(0),(1-7),(1-127),(1-255),(0-5),(2-255),(1-255),(1-7)");
        return;
    }
    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+CMUX: 0,0,5,");
        text_u32(&t, l->n1);
        text_str(&t, ",10,3,30,10,2");
        modem_answer_text(m, &t);
        return;
    }
    if (mode != MODEM_WRITE || l->cmux || m->chan != &m->ses.raw || !modem_arg_int(a, 0, 0, 0, &v) ||
        (a->count > 1 && a->v[1].str[0] != '\0' && !modem_arg_int(a, 1, 0, 0, &v)) ||
        (a->count > 2 && a->v[2].str[0] != '\0' && !modem_arg_int(a, 2, 1, 7, &v)) ||
        (a->count > 3 && a->v[3].str[0] != '\0' && !modem_arg_int(a, 3, 1, 1500, &n1))) {
        modem_error(m);
        return;
    }
    modem_ok(m);
    l->n1 = (uint8_t)(n1 > (int32_t)CMUX_MTU ? (int32_t)CMUX_MTU : n1);
    l->echo = m->ses.raw.echo;
    l->numeric = m->ses.raw.numeric;
    l->cmux = true;
    cmux_start(&l->mux, l->n1);
    chan_reset(&m->ses.raw, MODEM_CHAN_CLOSED, 1, l);
    modem_trace(m, MCLW_TRACE_STATE, modem_fourcc("CMUX"), 1, 0);
}

// 27.007 10.2.1.1: D*99[*[<called_address>][*[<L2P>][*[<cid>]]]]#; the
// context is the default one, 1, when the string names none.
static uint8_t dial_cid(const char *number) {
    const char *p = number + 3;
    uint32_t cid = 0;

    for (uint8_t stars = 0; stars < 3; p++) {
        if (*p == '\0' || *p == '#') {
            return 1;
        }
        if (*p == '*') {
            stars++;
        }
    }
    for (; *p >= '0' && *p <= '9' && cid < 256; p++) {
        cid = cid * 10 + (uint32_t)(*p - '0');
    }
    return cid >= 1 && cid <= 255 ? (uint8_t)cid : 1;
}

void modem_cmd_dial_ppp(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct modem_link *l = m->link;
    const char *number = modem_arg_str(a, 0);
    struct text t;

    (void)mode;
    if (l == NULL || m->net == NULL || number == NULL || strncmp(number, "*99", 3) != 0 || l->ppp_chan != NULL ||
        !modem_attached(m)) {
        modem_result(m, 3, "NO CARRIER");
        return;
    }
    text_init(&t);
    text_str(&t, "CONNECT ");
    text_u32(&t, m->set.baud);
    text_char(&t, '\0');
    modem_result(m, 1, t.buf);
    m->chan->mode = MODEM_CHAN_PPP;
    l->ppp_chan = m->chan;
    l->ppp_cid = dial_cid(number);
    ppp_open(&l->ppp);
}

void modem_link_bearer_lost(struct modem *m) {
    struct modem_link *l = m->link;

    if (l != NULL && l->ppp_chan != NULL) {
        ppp_terminate(&l->ppp, modem_now());
    }
}

bool modem_link_owns_socket(const struct modem *m, uint8_t sock) {
    return m->link != NULL && ip_owns_socket(&m->link->ip, sock);
}

void modem_link_net_notify(void *ctx, uint8_t sock, enum net_notice notice) {
    (void)ctx;
    (void)sock;
    (void)notice;
}

static void ppp_part_defaults(struct modem *m) {
    settings_defaults(&m->link->set);
}

static void ppp_part_clear(struct modem *m) {
    struct modem_link *l = m->link;

    memset(&l->stats, 0, sizeof(l->stats));
    memset(&l->mux.stats, 0, sizeof(l->mux.stats));
    memset(&l->ppp.stats, 0, sizeof(l->ppp.stats));
    memset(&l->ip.stats, 0, sizeof(l->ip.stats));
}

static void get_ip(const uint8_t ip[4], struct mclw_value *out) {
    for (size_t i = 0; i < 4; i++) {
        out->v[i] = ip[i];
    }
}

static int ppp_part_get(struct modem *m, size_t property, struct mclw_value *out) {
    const struct modem_link *l = m->link;

    switch (property) {
        case PROP_MRU:
            out->v[0] = l->set.mru;
            return 0;
        case PROP_GW:
            get_ip(l->set.gw, out);
            return 0;
        case PROP_DNS:
            get_ip(l->set.dns, out);
            return 0;
        case PROP_CMUX:
            out->v[0] = l->cmux;
            return 0;
        case PROP_PPP_UP:
            out->v[0] = l->ppp.ipcp_open;
            return 0;
        case PROP_LCP_TERMINATES:
            out->v[0] = l->ppp.stats.terminates;
            return 0;
        case PROP_TCP_RETRANSMITS:
            out->v[0] = l->ip.stats.tcp_retransmits;
            return 0;
        case PROP_FRAMES_BAD_FCS:
            out->v[0] = l->mux.stats.bad_fcs + l->ppp.stats.bad_fcs;
            return 0;
        case PROP_DLCI_FC_STALLS:
            out->v[0] = l->stats.fc_stalls;
            return 0;
        default:
            return -EINVAL;
    }
}

static int ppp_part_set(struct modem *m, size_t property, const struct mclw_value *in) {
    struct modem_link *l = m->link;
    uint8_t *ip;

    switch (property) {
        case PROP_MRU:
            l->set.mru = (uint16_t)in->v[0];
            return 0;
        case PROP_GW:
            ip = l->set.gw;
            break;
        case PROP_DNS:
            ip = l->set.dns;
            break;
        default:
            return -EINVAL;
    }
    for (size_t i = 0; i < 4; i++) {
        ip[i] = (uint8_t)in->v[i];
    }
    return 0;
}

// The engine resets the link's session itself.
const struct modem_part ppp_part = {
    .property_count = PPP_PROPERTY_COUNT,
    .region_count = 0,
    .defaults = ppp_part_defaults,
    .clear = ppp_part_clear,
    .session = NULL,
    .get = ppp_part_get,
    .set = ppp_part_set,
    .region = NULL,
    .written = NULL,
};
