// SPDX-License-Identifier: Apache-2.0 OR MIT
// The A7672X's SSL dialect (manual chapter 19): the SSL contexts
// (CSSLCFG) and the CCH* sessions in manual receiving mode. A session is a
// plain connection to the fake network: the handshake and certificates are
// not modelled, and the services on 443 answer as on any other port.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/runtime.h"

#include "a7672x.h"
#include "modem.h"
#include "net.h"
#include "text.h"

#define NO_CTX 0xffu
#define CLIENT_TCP 1
#define CLIENT_TLS 2

// The piece of a CCHRECV answer read from a socket per call.
#define READ_CHUNK 64u

static uint8_t sock_of(int32_t sid) {
    return (uint8_t)(A7672X_LINKS + sid);
}

// A socket the PPP link's TCP holds reads as a closed session.
static uint8_t session_state(const struct a7672x *s, int32_t sid) {
    return a7672x_sock_free(s, sock_of(sid)) ? s->sockets[sock_of(sid)].state : NET_SOCK_CLOSED;
}

static uint32_t session_available(const struct a7672x *s, int32_t sid) {
    return session_state(s, sid) == NET_SOCK_OPEN ? net_available(&s->net, sock_of(sid)) : 0;
}

static void forget_session(struct a7672x *s, uint8_t sid) {
    static const uint8_t kinds[] = { A7672X_EV_CCHOPEN, A7672X_EV_CCHSEND, A7672X_EV_CCHEVENT,
        A7672X_EV_CCH_PEER_CLOSED, A7672X_EV_CCHCLOSE };

    for (size_t i = 0; i < sizeof(kinds); i++) {
        modem_event_cancel(&s->m, kinds[i], sid);
    }
    net_close(&s->net, sock_of(sid));
}

// Manual 19.4: each open session hears its peer close, then the service
// stops.
void a7672x_drop_sessions(struct a7672x *s) {
    for (uint8_t sid = 0; sid < A7672X_SESSIONS; sid++) {
        if (session_state(s, sid) == NET_SOCK_CLOSED) {
            continue;
        }
        forget_session(s, sid);
        net_abort(&s->net, sock_of(sid));
        s->ses.cch[sid].ssl_ctx = NO_CTX;
        modem_event(&s->m, 0, A7672X_EV_CCH_PEER_CLOSED, sid);
    }
    if (s->ses.cch_started) {
        s->ses.cch_started = false;
        modem_event(&s->m, 0, A7672X_EV_CCH_STOPPED, 0);
    }
}

static bool session_arg(struct modem *m, const struct modem_args *a, int32_t *sid) {
    if (!modem_arg_int(a, 0, 0, A7672X_SESSIONS - 1, sid)) {
        modem_error(m);
        return false;
    }
    return true;
}

// Manual 19.2.1. Only the <sslversion> and the SNI flag are kept, for the
// query form; the other settings are accepted and do nothing.
static void cmd_csslcfg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    static const char *const known[] = { "sslversion", "authmode", "ignorelocaltime", "negotiatetime", "cacert",
        "clientcert", "clientkey", "password", "enableSNI", "ignorecertCN", "dtlsnegotiateretry", "session_cache",
        "use_tickets" };
    struct a7672x *s = a7672x_of(m);
    struct text t;
    int32_t ctx;
    int32_t v;

    if (mode == MODEM_TEST) {
        modem_out_str(m, "\r\n+CSSLCFG: \"sslversion\",(0-9),(0-4)\r\n+CSSLCFG: \"authmode\",(0-9),(0-3)\r\n"
                         "+CSSLCFG: \"cacert\",(0-9),(5-108)\r\n+CSSLCFG: \"enableSNI\",(0-9),(0,1)\r\n");
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (a->count == 1) {
        if (!modem_arg_int(a, 0, 0, A7672X_SSL_CTX_MAX, &ctx)) {
            modem_error(m);
            return;
        }
        text_init(&t);
        text_str(&t, "+CSSLCFG: ");
        text_u32(&t, (uint32_t)ctx);
        text_char(&t, ',');
        text_u32(&t, s->ses.sslversion[ctx]);
        text_str(&t, ",0,1,300,\"\",\"\",\"\",\"\",");
        text_char(&t, (s->ses.sni & (1u << ctx)) != 0 ? '1' : '0');
        text_str(&t, ",0,0,0,0");
        modem_answer_text(m, &t);
        return;
    }
    const char *what = a->v[0].str;
    bool found = false;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        found = found || text_equal_nocase(what, known[i]);
    }
    if (!found || a->count != 3 || !modem_arg_int(a, 1, 0, A7672X_SSL_CTX_MAX, &ctx)) {
        modem_error(m);
        return;
    }
    if (text_equal_nocase(what, "sslversion")) {
        if (!modem_arg_int(a, 2, 0, 4, &v)) {
            modem_error(m);
            return;
        }
        s->ses.sslversion[ctx] = (uint8_t)v;
    } else if (text_equal_nocase(what, "enableSNI")) {
        if (!modem_arg_int(a, 2, 0, 1, &v)) {
            modem_error(m);
            return;
        }
        s->ses.sni = (uint16_t)(v != 0 ? s->ses.sni | (1u << ctx) : s->ses.sni & ~(1u << ctx));
    }
    modem_ok(m);
}

// Manual 19.2.5: before CCHSTART only. With <recv_mode> 0 a real module
// pushes received data unasked; that is not modelled, and the data waits
// for CCHRECV without a URC.
static void cmd_cchset(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;
    int32_t report;
    int32_t recv = 0;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CCHSET: (0,1),(0,1)");
    } else if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+CCHSET: ");
        text_u32(&t, s->ses.cch_report);
        text_char(&t, ',');
        text_u32(&t, s->ses.cch_recv_mode);
        modem_answer_text(m, &t);
    } else if (mode != MODEM_WRITE || s->ses.cch_started || !modem_arg_int(a, 0, 0, 1, &report) ||
        (a->count > 1 && !modem_arg_int(a, 1, 0, 1, &recv))) {
        modem_error(m);
    } else {
        s->ses.cch_report = (uint8_t)report;
        s->ses.cch_recv_mode = (uint8_t)recv;
        modem_ok(m);
    }
}

// Transparent mode is not modelled.
static void cmd_cchmode(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t v;

    if (mode == MODEM_READ) {
        modem_answer(m, "+CCHMODE: 0");
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CCHMODE: (0,1)");
    } else if (mode == MODEM_WRITE && !a7672x_of(m)->ses.cch_started && modem_arg_int(a, 0, 0, 0, &v)) {
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

static bool cid_form(enum modem_mode mode, struct modem_args *a) {
    int32_t cid;

    return mode == MODEM_EXEC || (mode == MODEM_WRITE && modem_arg_int(a, 0, 1, 1, &cid));
}

// Manual 19.2.7: OK and +CCHSTART: 0, or ERROR and the error; a second
// start without CCHSTOP is refused.
static void cmd_cchstart(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);

    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (!cid_form(mode, a) || s->ses.cch_started) {
        modem_error(m);
        return;
    }
    if (!a7672x_activate(s)) {
        modem_error(m);
        modem_event(m, 0, A7672X_EV_CCHSTART, A7672X_CCH_NONET);
        return;
    }
    s->ses.cch_started = true;
    modem_ok(m);
    modem_event(m, 0, A7672X_EV_CCHSTART, A7672X_CCH_OK);
}

static void cmd_cchstop(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);

    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (!cid_form(mode, a) || !s->ses.cch_started) {
        modem_error(m);
        return;
    }
    for (uint8_t sid = 0; sid < A7672X_SESSIONS; sid++) {
        forget_session(s, sid);
        s->ses.cch[sid].ssl_ctx = NO_CTX;
    }
    s->ses.cch_started = false;
    modem_ok(m);
    modem_event(m, 0, A7672X_EV_CCHSTOP, 0);
}

// Manual 19.2.9: the address without a <cid>, unlike +CGPADDR.
static void cmd_cchaddr(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;

    (void)a;
    if (mode != MODEM_EXEC || !s->ses.cch_started || !s->net.ses.bearer) {
        modem_error(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+CCHADDR: ");
    text_ip(&t, s->net.set.ip);
    modem_answer_text(m, &t);
}

static void cmd_cchsslcfg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;
    int32_t sid;
    int32_t ctx;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CCHSSLCFG: (0-1),(0-9)");
        return;
    }
    if (mode == MODEM_READ) {
        modem_out_str(m, "\r\n");
        for (uint8_t i = 0; i < A7672X_SESSIONS; i++) {
            text_init(&t);
            text_str(&t, "+CCHSSLCFG: ");
            text_u32(&t, i);
            text_char(&t, ',');
            if (s->ses.cch[i].ssl_ctx != NO_CTX) {
                text_u32(&t, s->ses.cch[i].ssl_ctx);
            }
            modem_info_line(m, &t);
        }
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE || !s->ses.cch_started || !modem_arg_int(a, 0, 0, A7672X_SESSIONS - 1, &sid) ||
        !modem_arg_int(a, 1, 0, A7672X_SSL_CTX_MAX, &ctx)) {
        modem_error(m);
        return;
    }
    s->ses.cch[sid].ssl_ctx = (uint8_t)ctx;
    modem_ok(m);
}

static void cchopen_list(struct modem *m) {
    const struct a7672x *s = a7672x_of(m);
    struct text t;

    modem_out_str(m, "\r\n");
    for (uint8_t sid = 0; sid < A7672X_SESSIONS; sid++) {
        const struct net_socket *sock = &s->sockets[sock_of(sid)];
        text_init(&t);
        text_str(&t, "+CCHOPEN: ");
        text_u32(&t, sid);
        if (sock->state == NET_SOCK_OPEN) {
            text_str(&t, ",\"");
            text_ip(&t, sock->ip);
            text_str(&t, "\",");
            text_u32(&t, sock->port);
            text_char(&t, ',');
            text_u32(&t, s->ses.cch[sid].client_type);
            text_char(&t, ',');
        } else {
            text_str(&t, ",\"\",,,");
        }
        modem_info_line(m, &t);
    }
    modem_ok(m);
}

// Manual 19.2.12: OK, then +CCHOPEN: <session>,<err>. DTLS (client type 3)
// is not modelled.
static void cmd_cchopen(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    int32_t sid;
    int32_t port;
    int32_t type = CLIENT_TLS;
    int32_t bind;
    uint8_t ip[4];

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CCHOPEN: (0-1),\"ADDRESS\",(1-65535)[,(1-3)[,(1-65535)]]");
        return;
    }
    if (mode == MODEM_READ) {
        cchopen_list(m);
        return;
    }
    if (mode != MODEM_WRITE || !session_arg(m, a, &sid)) {
        if (mode != MODEM_WRITE) {
            modem_error(m);
        }
        return;
    }
    const char *host = a->count > 1 ? a->v[1].str : "";
    size_t host_len = text_strnlen(host, NET_HOST_MAX + 1);
    if (!s->ses.cch_started || !a7672x_sock_free(s, sock_of(sid)) || session_state(s, sid) != NET_SOCK_CLOSED ||
        host_len == 0 ||
        host_len > NET_HOST_MAX || !modem_arg_int(a, 2, 1, 65535, &port) ||
        (a->count > 3 && !modem_arg_int(a, 3, CLIENT_TCP, CLIENT_TLS, &type)) ||
        (a->count > 4 && !modem_arg_int(a, 4, 1, 65535, &bind))) {
        modem_error(m);
        return;
    }
    struct a7672x_cch *c = &s->ses.cch[sid];
    uint8_t err = A7672X_CCH_OK;
    forget_session(s, (uint8_t)sid);
    if (!s->net.ses.bearer) {
        err = A7672X_CCH_NONET;
    } else if (!net_resolve(&s->net, host, ip)) {
        err = A7672X_CCH_DNS;
    } else if (!net_open(&s->net, sock_of(sid), false, ip, (uint16_t)port)) {
        err = A7672X_CCH_CONNECT;
    }
    c->open_err = err;
    c->client_type = (uint8_t)type;
    modem_trace(m, MCLW_TRACE_COMMAND, (uint32_t)sid, (uint16_t)(err == A7672X_CCH_OK ? port : 0), (int16_t)err);
    modem_ok(m);
    modem_event(m, 0, A7672X_EV_CCHOPEN, (uint8_t)sid);
}

// Manual 19.2.13: a session the server closed is still the DUT's to close.
static void cmd_cchclose(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    int32_t sid;

    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!session_arg(m, a, &sid)) {
        return;
    }
    if (session_state(s, sid) == NET_SOCK_CLOSED) {
        modem_error(m);
        return;
    }
    forget_session(s, (uint8_t)sid);
    s->ses.cch[sid].ssl_ctx = NO_CTX;
    modem_ok(m);
    modem_event(m, 0, A7672X_EV_CCHCLOSE, (uint8_t)sid);
}

static size_t cchsend_data(struct modem *m, const uint8_t *bytes, size_t len) {
    net_send(m->net, sock_of(m->ses.data_arg), bytes, len);
    return len;
}

static void cchsend_complete(struct modem *m) {
    struct a7672x *s = a7672x_of(m);
    uint8_t sid = m->ses.data_arg;

    // Manual 19.3.1: 4, peer closed.
    s->ses.cch[sid].send_err = session_state(s, sid) == NET_SOCK_OPEN ? A7672X_CCH_OK : 4;
    modem_ok(m);
    if (s->ses.cch_report != 0) {
        modem_event(m, 0, A7672X_EV_CCHSEND, sid);
    }
}

static const struct modem_phase cchsend_phase = {
    .data = cchsend_data,
    .complete = cchsend_complete,
    .expired = NULL,
};

// Manual 19.2.14: `>`, exactly <len> bytes ("otherwise, the serial port
// will be blocked"), OK, and +CCHSEND with CCHSET's report on.
static void cmd_cchsend(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    int32_t sid;
    int32_t len;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CCHSEND: (0-1),(1-2048)");
        return;
    }
    if (mode == MODEM_READ) {
        modem_answer(m, "+CCHSEND: 0,0,1,0");
        return;
    }
    if (mode != MODEM_WRITE || !session_arg(m, a, &sid)) {
        if (mode != MODEM_WRITE) {
            modem_error(m);
        }
        return;
    }
    if (session_state(s, sid) != NET_SOCK_OPEN || !modem_arg_int(a, 1, 1, A7672X_CCHSEND_MAX, &len)) {
        modem_error(m);
        return;
    }
    modem_prompt(m, &cchsend_phase, (uint16_t)len, (uint8_t)sid, 0);
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

// Manual 19.2.15: OK first, then one DATA chunk of at most what the output
// FIFO can take, then +CCHRECV: <session>,0. An empty cache is a bare
// ERROR.
static void cmd_cchrecv(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;
    int32_t sid;
    int32_t max = 0;

    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+CCHRECV: LEN");
        for (int32_t i = 0; i < A7672X_SESSIONS; i++) {
            text_char(&t, ',');
            text_u32(&t, session_available(s, i));
        }
        modem_answer_text(m, &t);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE || !session_arg(m, a, &sid)) {
        if (mode != MODEM_WRITE) {
            modem_error(m);
        }
        return;
    }
    uint32_t avail = session_available(s, sid);
    if ((a->count > 1 && !modem_arg_int(a, 1, 0, A7672X_CCHSEND_MAX, &max)) || avail == 0) {
        modem_error(m);
        return;
    }
    size_t overhead =
        sizeof("\r\nOK\r\n") + sizeof("\r\n+CCHRECV: DATA,1,2048\r\n") + sizeof("\r\n+CCHRECV: 1,0\r\n");
    size_t room = modem_out_free(m);
    size_t n = max == 0 || avail < (uint32_t)max ? avail : (size_t)max;
    if (n + overhead > room) {
        n = room > overhead ? room - overhead : 0;
    }
    if (n == 0) {
        modem_error(m);
        return;
    }
    modem_ok(m);
    text_init(&t);
    text_str(&t, "\r\n+CCHRECV: DATA,");
    text_u32(&t, (uint32_t)sid);
    text_char(&t, ',');
    text_u32(&t, (uint32_t)n);
    text_str(&t, "\r\n");
    modem_out_text(m, &t);
    emit(m, sock_of(sid), n);
    text_init(&t);
    text_str(&t, "+CCHRECV: ");
    text_u32(&t, (uint32_t)sid);
    text_str(&t, ",0");
    modem_info(m, &t);
}

void a7672x_ssl_event(struct a7672x *s, uint8_t kind, uint8_t arg) {
    struct text t;
    uint8_t sid = arg;

    text_init(&t);
    switch (kind) {
        case A7672X_EV_CCHSTART:
            text_str(&t, "+CCHSTART: ");
            text_u32(&t, arg);
            break;
        case A7672X_EV_CCHSTOP:
            text_str(&t, "+CCHSTOP: 0");
            break;
        case A7672X_EV_CCH_STOPPED:
            text_str(&t, "+CCH: CCH STOP");
            break;
        case A7672X_EV_CCHOPEN:
            text_str(&t, "+CCHOPEN: ");
            text_u32(&t, sid);
            text_char(&t, ',');
            text_u32(&t, s->ses.cch[sid].open_err);
            if (s->ses.cch[sid].open_err != A7672X_CCH_OK) {
                // Manual 19.2.10: a failed open clears the session's context.
                s->ses.cch[sid].ssl_ctx = NO_CTX;
            }
            break;
        case A7672X_EV_CCHSEND:
            text_str(&t, "+CCHSEND: ");
            text_u32(&t, sid);
            text_char(&t, ',');
            text_u32(&t, s->ses.cch[sid].send_err);
            break;
        case A7672X_EV_CCHEVENT:
            if (s->ses.cch_recv_mode != 1 || session_available(s, sid) == 0) {
                return;
            }
            text_str(&t, "+CCHEVENT: ");
            text_u32(&t, sid);
            text_str(&t, ",RECV EVENT");
            break;
        case A7672X_EV_CCH_PEER_CLOSED:
            if (session_state(s, sid) != NET_SOCK_REMOTE_CLOSED) {
                return;
            }
            text_str(&t, "+CCH_PEER_CLOSED: ");
            text_u32(&t, sid);
            break;
        case A7672X_EV_CCHCLOSE:
            text_str(&t, "+CCHCLOSE: ");
            text_u32(&t, sid);
            text_str(&t, ",0");
            break;
        default:
            return;
    }
    modem_info(&s->m, &t);
}

static const struct modem_command commands[] = {
    { "+CSSLCFG", cmd_csslcfg },
    { "+CCERTDOWN", modem_cmd_unsupported },
    { "+CCHSET", cmd_cchset },
    { "+CCHMODE", cmd_cchmode },
    { "+CCHSTART", cmd_cchstart },
    { "+CCHSTOP", cmd_cchstop },
    { "+CCHADDR", cmd_cchaddr },
    { "+CCHSSLCFG", cmd_cchsslcfg },
    { "+CCHCFG", modem_cmd_accept },
    { "+CCHOPEN", cmd_cchopen },
    { "+CCHCLOSE", cmd_cchclose },
    { "+CCHSEND", cmd_cchsend },
    { "+CCHRECV", cmd_cchrecv },
};

const struct modem_commands a7672x_ssl_commands = MODEM_COMMANDS(commands);
