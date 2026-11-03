// SPDX-License-Identifier: Apache-2.0 OR MIT
// The Sequans Monarch's socket dialect: the SQNS* commands of LR5 chapter
// 12 in command mode, translated to the fake network's sockets; <connId>
// n is socket n - 1. A socket the PPP link's TCP holds is none of theirs.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/runtime.h"

#include "modem.h"
#include "net.h"
#include "sequans_monarch.h"
#include "text.h"

// The piece of a +SQNSRECV answer read from a socket per call.
#define READ_CHUNK 64u
// The bytes a +SQNSSENDEXT hex phase decodes before handing them on.
#define SEND_CHUNK 32u
// The local port +SQNSS reports for socket n is this plus n.
#define LOCAL_PORT_BASE 40000u

// LR5 12.12 <state>.
enum {
    STATE_CLOSED = 0,
    STATE_SUSPENDED = 2,
    STATE_SUSPENDED_DATA = 3,
};

void sequans_monarch_socket_defaults(struct sequans_monarch_socket_cfg *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->cid = 1;
    cfg->pkt_size = 300;
    cfg->max_to = 90;
    cfg->conn_to = 600;
    cfg->tx_to = 50;
    cfg->profile = 1;
}

static uint8_t sock_state(const struct sequans_monarch *s, uint8_t sock) {
    if (modem_link_owns_socket(&s->m, sock)) {
        return NET_SOCK_CLOSED;
    }
    return s->sockets[sock].state;
}

void sequans_monarch_net_notify(void *ctx, uint8_t sock, enum net_notice notice) {
    struct sequans_monarch *s = ctx;

    if (modem_link_owns_socket(&s->m, sock)) {
        return;
    }
    modem_event(&s->m, 0, notice == NET_NOTICE_DATA ? SEQUANS_MONARCH_EV_SQNSRING : SEQUANS_MONARCH_EV_SQNSH, sock);
}

// A socket's URCs that have not gone out yet belong to it alone, not to
// the next dial on the same <connId>.
static void forget_socket(struct sequans_monarch *s, uint8_t sock) {
    modem_event_cancel(&s->m, SEQUANS_MONARCH_EV_SQNSRING, sock);
    modem_event_cancel(&s->m, SEQUANS_MONARCH_EV_SQNSH, sock);
    net_close(&s->net, sock);
}

void sequans_monarch_drop_sockets(struct sequans_monarch *s) {
    for (uint8_t sock = 0; sock < SEQUANS_MONARCH_SOCKETS; sock++) {
        if (sock_state(s, sock) != NET_SOCK_OPEN) {
            continue;
        }
        forget_socket(s, sock);
        net_abort(&s->net, sock);
        modem_event(&s->m, 0, SEQUANS_MONARCH_EV_SQNSH, sock);
    }
}

// +SQNSRING carries what is buffered when it goes out, at most what one
// +SQNSRECV can take; +SQNSH only for a socket the server closed.
void sequans_monarch_socket_event(struct sequans_monarch *s, uint8_t kind, uint8_t arg) {
    struct text t;

    text_init(&t);
    if (kind == SEQUANS_MONARCH_EV_SQNSRING) {
        uint32_t n = net_available(&s->net, arg);
        if (sock_state(s, arg) != NET_SOCK_OPEN || n == 0) {
            return;
        }
        text_str(&t, "+SQNSRING: ");
        text_u32(&t, arg + 1u);
        if (s->nv.sockets[arg].sr_mode != 0) {
            text_char(&t, ',');
            text_u32(&t, n < SEQUANS_MONARCH_DATA_MAX ? n : SEQUANS_MONARCH_DATA_MAX);
        }
    } else if (kind == SEQUANS_MONARCH_EV_SQNSH) {
        if (sock_state(s, arg) != NET_SOCK_REMOTE_CLOSED) {
            return;
        }
        text_str(&t, "+SQNSH: ");
        text_u32(&t, arg + 1u);
    } else {
        return;
    }
    modem_info(&s->m, &t);
}

// <connId> as the socket index, ERROR when out of range.
static bool conn_id(struct modem *m, const struct modem_args *a, uint8_t *sock) {
    int32_t id;

    if (!modem_arg_int(a, 0, 1, SEQUANS_MONARCH_SOCKETS, &id)) {
        modem_error(m);
        return false;
    }
    *sock = (uint8_t)(id - 1);
    return true;
}

// An optional parameter: absent or empty leaves `out` as it is.
static bool opt_int(const struct modem_args *a, uint8_t i, int32_t lo, int32_t hi, int32_t *out) {
    if (i >= a->count || (!a->v[i].quoted && a->v[i].str[0] == '\0')) {
        return true;
    }
    return modem_arg_int(a, i, lo, hi, out);
}

// The list forms: one line per <connId>.
static void list(struct modem *m, void (*line)(struct modem *m, uint8_t sock, struct text *t)) {
    struct text t;

    modem_out_str(m, "\r\n");
    for (uint8_t sock = 0; sock < SEQUANS_MONARCH_SOCKETS; sock++) {
        text_init(&t);
        line(m, sock, &t);
        modem_info_line(m, &t);
    }
    modem_ok(m);
}

static void sqnscfg_line(struct modem *m, uint8_t sock, struct text *t) {
    const struct sequans_monarch_socket_cfg *c = &sequans_monarch_of(m)->nv.sockets[sock];

    text_str(t, "+SQNSCFG: ");
    text_u32(t, sock + 1u);
    text_char(t, ',');
    text_u32(t, c->cid);
    text_char(t, ',');
    text_u32(t, c->pkt_size);
    text_char(t, ',');
    text_u32(t, c->max_to);
    text_char(t, ',');
    text_u32(t, c->conn_to);
    text_char(t, ',');
    text_u32(t, c->tx_to);
}

// LR5 12.2: stored per socket; <cid> picks the context SQNSD dials on.
// The timeouts are not modelled.
static void cmd_sqnscfg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch_socket_cfg *c;
    int32_t v[5];
    uint8_t sock;

    if (mode == MODEM_READ) {
        list(m, sqnscfg_line);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_answer(m, "+SQNSCFG: (1-6),(0-6),(0-1500),(0-65535),(0,10-1200),(0-255)");
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!conn_id(m, a, &sock)) {
        return;
    }
    if (!modem_arg_int(a, 1, 0, SEQUANS_MONARCH_CONTEXTS, &v[0]) || !modem_arg_int(a, 2, 0, 1500, &v[1]) ||
        !modem_arg_int(a, 3, 0, 65535, &v[2]) || !modem_arg_int(a, 4, 0, 1200, &v[3]) ||
        (v[3] != 0 && v[3] < 10) || !modem_arg_int(a, 5, 0, 255, &v[4])) {
        modem_error(m);
        return;
    }
    c = &sequans_monarch_of(m)->nv.sockets[sock];
    c->cid = (uint8_t)v[0];
    c->pkt_size = (uint16_t)v[1];
    c->max_to = (uint16_t)v[2];
    c->conn_to = (uint16_t)v[3];
    c->tx_to = (uint8_t)v[4];
    modem_ok(m);
}

static void sqnscfgext_line(struct modem *m, uint8_t sock, struct text *t) {
    const struct sequans_monarch_socket_cfg *c = &sequans_monarch_of(m)->nv.sockets[sock];

    text_str(t, "+SQNSCFGEXT: ");
    text_u32(t, sock + 1u);
    text_char(t, ',');
    text_u32(t, c->sr_mode);
    text_char(t, ',');
    text_u32(t, c->recv_mode);
    text_char(t, ',');
    text_u32(t, c->keepalive);
    text_char(t, ',');
    text_u32(t, c->listen_auto);
    text_char(t, ',');
    text_u32(t, c->send_mode);
    text_str(t, ",0,0");
}

// LR5 12.3: <srMode> 0 and 1 are modelled, 2 (the data in the URC) is
// not; <recvDataMode> shapes only that URC, so it is stored and nothing
// else.
static void cmd_sqnscfgext(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch_socket_cfg *c;
    int32_t sr;
    int32_t recv;
    int32_t keepalive;
    int32_t unused = 0;
    uint8_t sock;

    if (mode == MODEM_READ) {
        list(m, sqnscfgext_line);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_answer(m, "+SQNSCFGEXT: (1-6),(0-2),(0-1),(0-240),(0-1),(0-1),(0),(0)");
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!conn_id(m, a, &sock)) {
        return;
    }
    c = &sequans_monarch_of(m)->nv.sockets[sock];
    int32_t listen = c->listen_auto;
    int32_t send = c->send_mode;
    if (!modem_arg_int(a, 1, 0, 2, &sr) || !modem_arg_int(a, 2, 0, 1, &recv) ||
        !modem_arg_int(a, 3, 0, 240, &keepalive) || !opt_int(a, 4, 0, 1, &listen) || !opt_int(a, 5, 0, 1, &send) ||
        !opt_int(a, 6, 0, 0, &unused) || !opt_int(a, 7, 0, 0, &unused)) {
        modem_error(m);
        return;
    }
    if (sr == 2) {
        modem_cme(m, MODEM_CME_NOT_SUPPORTED);
        return;
    }
    c->sr_mode = (uint8_t)sr;
    c->recv_mode = (uint8_t)recv;
    c->keepalive = (uint8_t)keepalive;
    c->listen_auto = (uint8_t)listen;
    c->send_mode = (uint8_t)send;
    modem_ok(m);
}

// LR5 16.14: a security profile is accepted and not kept; TLS sockets
// reach the same plaintext services.
static void cmd_sqnspcfg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t id;
    int32_t version = 0;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+SQNSPCFG: (1-6),(0-4),,(0-255),(1-19),(1-19),(1-19),");
    } else if (mode == MODEM_EXEC || (mode == MODEM_WRITE && modem_arg_int(a, 0, 1, SEQUANS_MONARCH_PROFILES, &id) &&
                                         (opt_int(a, 1, 0, 4, &version) || modem_arg_int(a, 1, 255, 255, &version)))) {
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

static void sqnsscfg_line(struct modem *m, uint8_t sock, struct text *t) {
    const struct sequans_monarch_socket_cfg *c = &sequans_monarch_of(m)->nv.sockets[sock];

    text_str(t, "+SQNSSCFG: ");
    text_u32(t, sock + 1u);
    text_char(t, ',');
    text_u32(t, c->tls);
    text_char(t, ',');
    text_u32(t, c->profile);
}

// LR5 12.13: only while the socket is not connected.
static void cmd_sqnsscfg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    int32_t enable;
    uint8_t sock;

    if (mode == MODEM_READ || mode == MODEM_EXEC) {
        list(m, sqnsscfg_line);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_answer(m, "+SQNSSCFG: (1-6),(0-1),(1-6)");
        return;
    }
    if (!conn_id(m, a, &sock)) {
        return;
    }
    int32_t profile = s->nv.sockets[sock].profile;
    if (!modem_arg_int(a, 1, 0, 1, &enable) || !opt_int(a, 2, 1, SEQUANS_MONARCH_PROFILES, &profile)) {
        modem_error(m);
        return;
    }
    if (sock_state(s, sock) == NET_SOCK_OPEN) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    s->nv.sockets[sock].tls = (uint8_t)enable;
    s->nv.sockets[sock].profile = (uint8_t)profile;
    modem_ok(m);
}

static void no_carrier(struct modem *m) {
    modem_out_str(m, "\r\nNO CARRIER\r\n");
}

// LR5 12.4 in command mode (<connMode> 1), set up synchronously: OK once
// connected, NO CARRIER when the socket's context is not up, the name does
// not resolve or nothing listens. Online mode and LR8.2's asynchronous
// <connSetup> are not modelled.
static void cmd_sqnsd(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    int32_t proto;
    int32_t port;
    int32_t closure = 0;
    int32_t local_port = 0;
    int32_t conn_mode = 0;
    int32_t any_remote = 0;
    int32_t setup = 0;
    uint8_t ip[4];
    uint8_t sock;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+SQNSD: (1-6),(0-1),(0-65535),,(0,255),(0-65535),(0-1),(0-2),(0-1)");
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!conn_id(m, a, &sock)) {
        return;
    }
    const char *host = modem_arg_str(a, 3);
    if (!modem_arg_int(a, 1, 0, 1, &proto) || !modem_arg_int(a, 2, 0, 65535, &port) || host == NULL ||
        host[0] == '\0' || strlen(host) > NET_HOST_MAX || !opt_int(a, 4, 0, 255, &closure) ||
        (closure != 0 && closure != 255) || !opt_int(a, 5, 0, 65535, &local_port) ||
        !opt_int(a, 6, 0, 1, &conn_mode) || !opt_int(a, 7, 0, 2, &any_remote) || !opt_int(a, 8, 0, 1, &setup)) {
        modem_error(m);
        return;
    }
    if (conn_mode != 1 || setup != 0) {
        modem_cme(m, MODEM_CME_NOT_SUPPORTED);
        return;
    }
    if (modem_link_owns_socket(m, sock) || sock_state(s, sock) == NET_SOCK_OPEN) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    forget_socket(s, sock);
    bool ok = sequans_monarch_context_up(s, s->nv.sockets[sock].cid) && net_resolve(&s->net, host, ip) &&
        net_open(&s->net, sock, proto == 1, ip, (uint16_t)port);
    modem_trace(m, MCLW_TRACE_COMMAND, sock + 1u, (uint16_t)(ok ? port : 0), ok ? 0 : -ECONNREFUSED);
    if (!ok) {
        no_carrier(m);
        return;
    }
    memset(&s->ses.conns[sock], 0, sizeof(s->ses.conns[sock]));
    modem_ok(m);
}

// LR5 12.5: closing a closed socket changes nothing.
static void cmd_sqnsh(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    uint8_t sock;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+SQNSH: (1-6)");
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!conn_id(m, a, &sock)) {
        return;
    }
    if (!modem_link_owns_socket(m, sock)) {
        forget_socket(s, sock);
    }
    modem_ok(m);
}

static uint32_t buffered(const struct sequans_monarch *s, uint8_t sock) {
    return sock_state(s, sock) == NET_SOCK_OPEN ? net_available(&s->net, sock) : 0;
}

static void sqnsi_line(struct modem *m, uint8_t sock, struct text *t) {
    const struct sequans_monarch *s = sequans_monarch_of(m);
    const struct sequans_monarch_conn *c = &s->ses.conns[sock];
    uint32_t in = buffered(s, sock);
    bool ours = !modem_link_owns_socket(m, sock);

    text_str(t, "+SQNSI: ");
    text_u32(t, sock + 1u);
    text_char(t, ',');
    text_u32(t, ours ? c->sent : 0);
    text_char(t, ',');
    text_u32(t, ours ? c->read + in : 0);
    text_char(t, ',');
    text_u32(t, in);
    text_str(t, ",0");
}

// LR5 12.6: all five fields; <received> counts what has arrived, read or
// not, and nothing waits for an acknowledgement.
static void cmd_sqnsi(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;
    uint8_t sock;

    if (mode == MODEM_EXEC) {
        list(m, sqnsi_line);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_answer(m, "+SQNSI: (1-6)");
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (conn_id(m, a, &sock)) {
        text_init(&t);
        sqnsi_line(m, sock, &t);
        modem_answer_text(m, &t);
    }
}

// LR5 12.12: a socket dialled in command mode is suspended, with pending
// data or without; one the server closed reads closed once drained.
static void sqnss_line(struct modem *m, uint8_t sock, struct text *t) {
    const struct sequans_monarch *s = sequans_monarch_of(m);
    const struct net_socket *ns = &s->sockets[sock];

    text_str(t, "+SQNSS: ");
    text_u32(t, sock + 1u);
    if (sock_state(s, sock) != NET_SOCK_OPEN) {
        text_str(t, ",0");
        return;
    }
    text_str(t, net_available(&s->net, sock) > 0 ? ",3," : ",2,");
    text_ip(t, s->net.set.ip);
    text_char(t, ',');
    text_u32(t, LOCAL_PORT_BASE + sock + 1u);
    text_char(t, ',');
    text_ip(t, ns->ip);
    text_char(t, ',');
    text_u32(t, ns->port);
}

static void cmd_sqnss(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode == MODEM_EXEC || mode == MODEM_READ) {
        list(m, sqnss_line);
    } else if (mode == MODEM_TEST) {
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

static void emit(struct modem *m, uint8_t sock, size_t n) {
    uint8_t buf[READ_CHUNK];

    while (n > 0) {
        size_t got = net_read(m->net, sock, buf, n < sizeof(buf) ? n : sizeof(buf));
        if (got == 0) {
            return;
        }
        modem_out(m, buf, got);
        n -= got;
    }
}

// LR5 12.10: `+SQNSRECV: <connId>,<n>`, CR LF, n raw bytes, CR LF, OK; n is
// clamped to what the output FIFO can take. Nothing buffered is an error.
static void cmd_sqnsrecv(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    struct text t;
    int32_t want;
    uint8_t sock;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+SQNSRECV: (1-6),(1-1500)");
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!conn_id(m, a, &sock)) {
        return;
    }
    if (!modem_arg_int(a, 1, 1, SEQUANS_MONARCH_DATA_MAX, &want)) {
        modem_error(m);
        return;
    }
    size_t overhead = sizeof("\r\n+SQNSRECV: 6,1500\r\n") + sizeof("\r\n\r\nOK\r\n");
    size_t room = modem_out_free(m);
    size_t n = buffered(s, sock);
    if (n > (size_t)want) {
        n = (size_t)want;
    }
    if (n + overhead > room) {
        n = room > overhead ? room - overhead : 0;
    }
    if (n == 0) {
        modem_error(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+SQNSRECV: ");
    text_u32(&t, sock + 1u);
    text_char(&t, ',');
    text_u32(&t, (uint32_t)n);
    modem_info(m, &t);
    emit(m, sock, n);
    s->ses.conns[sock].read += (uint32_t)n;
    modem_out_str(m, "\r\n");
    modem_ok(m);
}

static void send_bytes(struct modem *m, const uint8_t *bytes, size_t len) {
    uint8_t sock = m->ses.data_arg;

    net_send(m->net, sock, bytes, len);
    sequans_monarch_of(m)->ses.conns[sock].sent += (uint32_t)len;
}

static size_t raw_data(struct modem *m, const uint8_t *bytes, size_t len) {
    send_bytes(m, bytes, len);
    return len;
}

static int hex_value(uint8_t c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// <sendDataMode> 1: two hex digits a byte. TinyGSM follows every pair with
// a NUL, so NUL, CR and LF between digits are skipped; any other byte that
// is not a digit ends the phase with ERROR, the bytes before it sent.
static size_t hex_data(struct modem *m, const uint8_t *bytes, size_t len) {
    struct sequans_monarch_session *ses = &sequans_monarch_of(m)->ses;
    uint8_t out[SEND_CHUNK];
    size_t n = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = bytes[i];
        if (b == 0 || b == '\r' || b == '\n') {
            continue;
        }
        int v = hex_value(b);
        if (v < 0) {
            send_bytes(m, out, n);
            modem_phase_end(m);
            modem_error(m);
            return i + 1;
        }
        if (!ses->hex_half) {
            ses->hex_high = (uint8_t)v;
            ses->hex_half = true;
            continue;
        }
        ses->hex_half = false;
        out[n++] = (uint8_t)((ses->hex_high << 4) | v);
        if (--ses->send_left == 0) {
            send_bytes(m, out, n);
            modem_phase_end(m);
            modem_ok(m);
            return i + 1;
        }
        if (n == sizeof(out)) {
            send_bytes(m, out, n);
            n = 0;
        }
    }
    send_bytes(m, out, n);
    return len;
}

static void send_complete(struct modem *m) {
    modem_ok(m);
}

static void send_expired(struct modem *m) {
    modem_trace(m, MCLW_TRACE_ERROR, modem_fourcc("SQNS"), 0, -ETIMEDOUT);
    modem_error(m);
}

static const struct modem_phase raw_phase = {
    .data = raw_data,
    .complete = send_complete,
    .expired = send_expired,
};

static const struct modem_phase hex_phase = {
    .data = hex_data,
    .complete = NULL,
    .expired = send_expired,
};

// LR5 12.15: the `> ` prompt, then <bytesToSend> bytes, raw or as hex as
// +SQNSCFGEXT <sendDataMode> says, then OK.
static void cmd_sqnssendext(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    int32_t len;
    uint8_t sock;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+SQNSSENDEXT: (1-6),(1-1500)");
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!conn_id(m, a, &sock)) {
        return;
    }
    if (!modem_arg_int(a, 1, 1, SEQUANS_MONARCH_DATA_MAX, &len)) {
        modem_error(m);
        return;
    }
    if (sock_state(s, sock) != NET_SOCK_OPEN) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    if (s->nv.sockets[sock].send_mode == 0) {
        modem_prompt(m, &raw_phase, (uint16_t)len, sock, SEQUANS_MONARCH_SEND_INPUT_MS);
        return;
    }
    s->ses.send_left = (uint16_t)len;
    s->ses.hex_half = false;
    modem_prompt(m, &hex_phase, 0, sock, SEQUANS_MONARCH_SEND_INPUT_MS);
}

static const struct modem_command commands[] = {
    { "+SQNSCFG", cmd_sqnscfg },
    { "+SQNSCFGEXT", cmd_sqnscfgext },
    { "+SQNSPCFG", cmd_sqnspcfg },
    { "+SQNSSCFG", cmd_sqnsscfg },
    { "+SQNSD", cmd_sqnsd },
    { "+SQNSH", cmd_sqnsh },
    { "+SQNSI", cmd_sqnsi },
    { "+SQNSS", cmd_sqnss },
    { "+SQNSRECV", cmd_sqnsrecv },
    { "+SQNSSENDEXT", cmd_sqnssendext },
};

const struct modem_commands sequans_monarch_socket_commands = MODEM_COMMANDS(commands);
