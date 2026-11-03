// SPDX-License-Identifier: Apache-2.0 OR MIT
// The SIM7080G's socket dialect: DNS and the CA* commands (manual chapters
// 19 and 12), translated to the fake network's sockets, one per cid.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/runtime.h"

#include "modem.h"
#include "net.h"
#include "sim7080g.h"
#include "text.h"

// The piece of a +CARECV answer read from a socket per call.
#define READ_CHUNK 64u

void sim7080g_net_notify(void *ctx, uint8_t sock, enum net_notice notice) {
    struct sim7080g *s = ctx;

    modem_event(&s->m, 0, notice == NET_NOTICE_DATA ? SIM7080G_EV_DATAIND : SIM7080G_EV_CASTATE, sock);
}

static const struct net_socket *sock_at(const struct sim7080g *s, uint8_t cid) {
    return &s->sockets[cid];
}

static bool line_end(uint8_t b) {
    return b == '\r' || b == '\n';
}

// Zephyr's +CARECV handler stalls for good when a chunk's data starts with
// CR or LF (sim7080_sock.c). A chunk that leaves data behind is shortened
// so the next one starts on another byte; it keeps its length only when
// every byte after its first is CR or LF.
static size_t chunk_boundary(const struct sim7080g *s, uint8_t cid, size_t n) {
    if (n >= net_available(&s->net, cid)) {
        return n;
    }
    for (size_t cut = n; cut > 0; cut--) {
        if (!line_end(net_peek(&s->net, cid, (uint32_t)cut))) {
            return cut;
        }
    }
    return n;
}

static void queue_dataind(struct sim7080g *s, uint8_t cid) {
    modem_event(&s->m, 0, SIM7080G_EV_DATAIND, cid);
}

// A connection's URCs that have not gone out yet belong to it alone, not
// to the next one on the same cid.
static void forget_socket(struct sim7080g *s, uint8_t cid) {
    modem_event_cancel(&s->m, SIM7080G_EV_DATAIND, cid);
    modem_event_cancel(&s->m, SIM7080G_EV_CASTATE, cid);
    net_close(&s->net, cid);
}

void sim7080g_drop_sockets(struct sim7080g *s, uint32_t urc_delay_us) {
    for (uint8_t cid = 0; cid < SIM7080G_SOCKETS; cid++) {
        uint8_t state = sock_at(s, cid)->state;
        if (state == NET_SOCK_CLOSED) {
            continue;
        }
        forget_socket(s, cid);
        net_abort(&s->net, cid);
        if (state == NET_SOCK_OPEN) {
            modem_event(&s->m, urc_delay_us, SIM7080G_EV_CASTATE, cid);
        }
    }
}

void sim7080g_socket_event(struct sim7080g *s, uint8_t kind, uint8_t arg) {
    const struct net_socket *sock = sock_at(s, arg);
    struct text t;

    text_init(&t);
    if (kind == SIM7080G_EV_DATAIND) {
        if (sock->state == NET_SOCK_CLOSED || net_available(&s->net, arg) == 0) {
            return;
        }
        text_str(&t, "+CADATAIND: ");
        text_u32(&t, arg);
    } else if (kind == SIM7080G_EV_CASTATE) {
        if (sock->state != NET_SOCK_REMOTE_CLOSED) {
            return;
        }
        text_str(&t, "+CASTATE: ");
        text_u32(&t, arg);
        text_str(&t, ",0");
    } else {
        return;
    }
    modem_info(&s->m, &t);
}

static void caopen_result(struct modem *m, int32_t cid, uint8_t result) {
    struct text t;

    text_init(&t);
    text_str(&t, "+CAOPEN: ");
    text_u32(&t, (uint32_t)cid);
    text_char(&t, ',');
    text_u32(&t, result);
    modem_info(m, &t);
    modem_ok(m);
}

static bool socket_cid(struct modem *m, const struct modem_args *a, int32_t *cid) {
    if (!modem_arg_int(a, 0, 0, SIM7080G_CID_MAX, cid)) {
        modem_error(m);
        return false;
    }
    if (*cid >= SIM7080G_SOCKETS || sock_at(sim7080g_of(m), (uint8_t)*cid)->state == NET_SOCK_CLOSED) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return false;
    }
    return true;
}

// Manual 12.2.3 read form, one line per open connection. The server is the
// address it resolved to; the name is not kept.
static void caopen_list(struct modem *m) {
    const struct sim7080g *s = sim7080g_of(m);
    struct text t;
    bool any = false;

    for (uint8_t cid = 0; cid < SIM7080G_SOCKETS; cid++) {
        const struct net_socket *sock = sock_at(s, cid);
        if (sock->state != NET_SOCK_OPEN) {
            continue;
        }
        if (!any) {
            modem_out_str(m, "\r\n");
            any = true;
        }
        text_init(&t);
        text_str(&t, "+CAOPEN: ");
        text_u32(&t, cid);
        text_str(&t, sock->udp ? ",0,\"UDP\",\"" : ",0,\"TCP\",\"");
        text_ip(&t, sock->ip);
        text_str(&t, "\",");
        text_u32(&t, sock->port);
        modem_info_line(m, &t);
    }
    modem_ok(m);
}

// Manual 12.2.3, synchronous form: +CAOPEN: <cid>,<result> before OK.
// TLS set with CASSLCFG is accepted and the same plaintext services answer.
static void cmd_caopen(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7080g *s = sim7080g_of(m);
    int32_t cid;
    int32_t pdp;
    int32_t port;
    int32_t recv_mode = 0;
    uint8_t ip[4];

    if (mode == MODEM_TEST) {
        modem_info_str(m, "+CAOPEN: (0-12),(0-4),(\"TCP\",\"UDP\",\"NONIP\"),64,(1-65535),(0,1)");
        modem_ok(m);
        return;
    }
    if (mode == MODEM_READ) {
        caopen_list(m);
        return;
    }
    if (mode != MODEM_WRITE || a->count < 5 || !modem_arg_int(a, 0, 0, SIM7080G_CID_MAX, &cid) ||
        !modem_arg_int(a, 1, 0, SIM7080G_PDP_CONTEXTS - 1, &pdp)) {
        modem_error(m);
        return;
    }
    const char *type = a->v[2].str;
    const char *host = a->v[3].str;
    bool udp = text_equal_nocase(type, "UDP");
    uint8_t result = SIM7080G_CAOPEN_OK;

    if (cid >= SIM7080G_SOCKETS) {
        result = SIM7080G_CAOPEN_CONNECTION_LIMIT;
    } else if (sock_at(s, (uint8_t)cid)->state == NET_SOCK_REMOTE_CLOSED) {
        result = SIM7080G_CAOPEN_NOT_RELEASED;
    } else if (sock_at(s, (uint8_t)cid)->state != NET_SOCK_CLOSED) {
        result = SIM7080G_CAOPEN_ILLEGAL_OPERATION;
    } else if (!udp && !text_equal_nocase(type, "TCP")) {
        result = text_equal_nocase(type, "NONIP") ? SIM7080G_CAOPEN_NOT_SUPPORTED : SIM7080G_CAOPEN_PARAMETER_INVALID;
    } else if (!modem_arg_int(a, 4, 1, 65535, &port) || strlen(host) == 0 || strlen(host) > NET_HOST_MAX ||
        (a->count > 5 && !modem_arg_int(a, 5, 0, 1, &recv_mode))) {
        result = SIM7080G_CAOPEN_PARAMETER_INVALID;
    } else if (recv_mode != 0) {
        result = SIM7080G_CAOPEN_NOT_SUPPORTED;
    } else if (pdp != 0 || !s->net.ses.bearer) {
        result = SIM7080G_CAOPEN_NETWORK_NOT_ACTIVE;
    } else if (!net_resolve(&s->net, host, ip)) {
        result = SIM7080G_CAOPEN_DNS_FAILED;
    } else {
        forget_socket(s, (uint8_t)cid);
        if (!net_open(&s->net, (uint8_t)cid, udp, ip, (uint16_t)port)) {
            result = SIM7080G_CAOPEN_REMOTE_REFUSED;
        }
    }
    modem_trace(m, MCLW_TRACE_COMMAND, (uint32_t)cid, (uint16_t)(result == SIM7080G_CAOPEN_OK ? port : 0),
        (int16_t)result);
    caopen_result(m, cid, result);
}

static size_t casend_data(struct modem *m, const uint8_t *bytes, size_t len) {
    net_send(m->net, m->ses.data_arg, bytes, len);
    return len;
}

static void casend_complete(struct modem *m) {
    modem_defer_ok(m, SIM7080G_CASEND_OK_DELAY_US);
}

// Manual 12.2.5: a CASEND data phase ends after <inputtime>; whatever
// arrived has been sent and the command fails.
static void casend_expired(struct modem *m) {
    modem_trace(m, MCLW_TRACE_ERROR, modem_fourcc("CASE"), 0, -ETIMEDOUT);
    modem_error(m);
}

static const struct modem_phase casend_phase = {
    .data = casend_data,
    .complete = casend_complete,
    .expired = casend_expired,
};

static void cmd_casend(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;
    int32_t cid;
    int32_t len;
    int32_t input_ms = SIM7080G_CASEND_INPUT_MS;

    if (mode == MODEM_TEST) {
        modem_info_str(m, "+CASEND: (0-12),(1-1460),(100-10000)");
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!socket_cid(m, a, &cid)) {
        return;
    }
    if (sock_at(sim7080g_of(m), (uint8_t)cid)->state != NET_SOCK_OPEN) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    if (a->count == 1) {
        text_init(&t);
        text_str(&t, "+CASEND: ");
        text_u32(&t, SIM7080G_CASEND_MAX);
        modem_info(m, &t);
        modem_ok(m);
        return;
    }
    if (!modem_arg_int(a, 1, 1, SIM7080G_CASEND_MAX, &len) ||
        (a->count > 2 && !modem_arg_int(a, 2, 100, 10000, &input_ms))) {
        modem_error(m);
        return;
    }
    modem_prompt(m, &casend_phase, (uint16_t)len, (uint8_t)cid, (uint32_t)input_ms);
}

static void emit(struct modem *m, uint8_t cid, size_t n) {
    uint8_t buf[READ_CHUNK];

    while (n > 0) {
        size_t got = net_read(m->net, cid, buf, n < sizeof(buf) ? n : sizeof(buf));
        if (got == 0) {
            return;
        }
        modem_out(m, buf, got);
        n -= got;
    }
}

// `+CARECV: <n>,<data>`, never with n of 0 (the Zephyr driver's handler
// loops on it), clamped to what the output FIFO can take.
static void cmd_carecv(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7080g *s = sim7080g_of(m);
    struct text t;
    int32_t cid;
    int32_t want;

    if (mode == MODEM_TEST) {
        modem_info_str(m, "+CARECV: (0-12),(1-1460)");
        modem_ok(m);
        return;
    }
    if (mode == MODEM_READ) {
        bool any = false;
        for (uint8_t i = 0; i < SIM7080G_SOCKETS; i++) {
            uint32_t n = net_available(&s->net, i);
            if (sock_at(s, i)->state == NET_SOCK_CLOSED || n == 0) {
                continue;
            }
            if (!any) {
                modem_out_str(m, "\r\n");
                any = true;
            }
            text_init(&t);
            text_str(&t, "+CARECV: ");
            text_u32(&t, i);
            text_char(&t, ',');
            text_u32(&t, n);
            modem_info_line(m, &t);
        }
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!socket_cid(m, a, &cid)) {
        return;
    }
    if (!modem_arg_int(a, 1, 1, INT32_MAX, &want)) {
        modem_error(m);
        return;
    }
    size_t overhead = sizeof("\r\n+CARECV: 0000,") + sizeof("\r\n\r\nOK\r\n");
    size_t room = modem_out_free(m);
    size_t n = net_available(&s->net, (uint8_t)cid);
    if (n > (size_t)want) {
        n = (size_t)want;
    }
    if (n > SIM7080G_CASEND_MAX) {
        n = SIM7080G_CASEND_MAX;
    }
    if (n + overhead > room) {
        n = room > overhead ? room - overhead : 0;
    }
    if (n == 0) {
        if (net_available(&s->net, (uint8_t)cid) > 0) {
            queue_dataind(s, (uint8_t)cid);
        }
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    n = chunk_boundary(s, (uint8_t)cid, n);
    text_init(&t);
    text_str(&t, "\r\n+CARECV: ");
    text_u32(&t, (uint32_t)n);
    text_char(&t, ',');
    modem_out_text(m, &t);
    emit(m, (uint8_t)cid, n);
    modem_out_str(m, "\r\n");
    modem_ok(m);
    if (net_available(&s->net, (uint8_t)cid) > 0) {
        queue_dataind(s, (uint8_t)cid);
    }
}

static void cmd_castate(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    const struct sim7080g *s = sim7080g_of(m);
    struct text t;
    bool any = false;

    (void)a;
    if (mode != MODEM_READ) {
        modem_error(m);
        return;
    }
    for (uint8_t i = 0; i < SIM7080G_SOCKETS; i++) {
        uint8_t state = sock_at(s, i)->state;
        if (state == NET_SOCK_CLOSED) {
            continue;
        }
        if (!any) {
            modem_out_str(m, "\r\n");
            any = true;
        }
        text_init(&t);
        text_str(&t, "+CASTATE: ");
        text_u32(&t, i);
        text_str(&t, state == NET_SOCK_OPEN ? ",1" : ",0");
        modem_info_line(m, &t);
    }
    modem_ok(m);
}

static void cmd_caclose(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t cid;

    if (mode == MODEM_TEST) {
        modem_info_str(m, "+CACLOSE: (0-12)");
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!socket_cid(m, a, &cid)) {
        return;
    }
    forget_socket(sim7080g_of(m), (uint8_t)cid);
    modem_ok(m);
}

static void cmd_cacid(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t cid;

    if (mode == MODEM_WRITE && !modem_arg_int(a, 0, 0, SIM7080G_CID_MAX, &cid)) {
        modem_error(m);
        return;
    }
    modem_ok(m);
}

// TinyGSM checks for the `+CSSLCFG:` line of the "ctxindex" form
// (manual 11.2.1); the other forms only configure.
static void cmd_csslcfg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7080g *s = sim7080g_of(m);
    struct text t;
    int32_t ctx;
    int32_t version;

    if (mode != MODEM_WRITE || a->count < 2) {
        modem_ok(m);
        return;
    }
    const char *what = a->v[0].str;
    if (text_equal_nocase(what, "SSLVERSION") && modem_arg_int(a, 2, 0, 5, &version)) {
        s->ses.sslversion = (uint8_t)version;
    } else if (text_equal_nocase(what, "CTXINDEX")) {
        if (!modem_arg_int(a, 1, 0, 5, &ctx)) {
            modem_error(m);
            return;
        }
        text_init(&t);
        text_str(&t, "+CSSLCFG: ");
        text_u32(&t, (uint32_t)ctx);
        text_char(&t, ',');
        text_u32(&t, s->ses.sslversion);
        text_str(&t, ",0x0035,0,1,\"\"");
        modem_info(m, &t);
    }
    modem_ok(m);
}

// Manual 19.2.3: OK, then the answer as a URC. An unknown name is a
// failure URC, never ERROR: the Zephyr driver waits 210 s for the URC.
static void cmd_cdnsgip(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7080g *s = sim7080g_of(m);
    struct sim7080g_session *ses = &s->ses;

    if (mode == MODEM_TEST) {
        modem_info_str(m, "+CDNSGIP: 65,(0-10),(0-60000)");
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE || a->count < 1) {
        modem_error(m);
        return;
    }
    const char *host = a->v[0].str;
    size_t len = text_strnlen(host, NET_HOST_MAX + 1);
    size_t keep = len > NET_HOST_MAX ? NET_HOST_MAX : len;

    memcpy(ses->dns_host, host, keep);
    ses->dns_host[keep] = '\0';
    ses->dns_ok = false;
    if (len == 0 || len > NET_HOST_MAX) {
        ses->dns_err = SIM7080G_DNS_INVALID_PARA;
    } else if (!s->net.ses.bearer) {
        ses->dns_err = SIM7080G_DNS_NETWORK_ERROR;
    } else if (net_resolve(&s->net, host, ses->dns_ip)) {
        ses->dns_ok = true;
    } else {
        ses->dns_err = SIM7080G_DNS_ERROR_UNKNOWN;
    }
    modem_ok(m);
    modem_event(m, SIM7080G_URC_DELAY_US, SIM7080G_EV_DNS, 0);
}

static const struct modem_command commands[] = {
    { "+CDNSGIP", cmd_cdnsgip },
    { "+CACID", cmd_cacid },
    { "+CASSLCFG", modem_cmd_accept },
    { "+CSSLCFG", cmd_csslcfg },
    { "+CACFG", modem_cmd_accept },
    { "+CAOPEN", cmd_caopen },
    { "+CASEND", cmd_casend },
    { "+CARECV", cmd_carecv },
    { "+CASTATE", cmd_castate },
    { "+CACLOSE", cmd_caclose },
};

const struct modem_commands sim7080g_socket_commands = MODEM_COMMANDS(commands);
