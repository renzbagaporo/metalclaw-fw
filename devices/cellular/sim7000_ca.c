// SPDX-License-Identifier: Apache-2.0 OR MIT
// The SIM7000's CA socket stack (manual chapters 17 and 18), as TinyGSM's
// SIM7000SSL driver speaks it, translated to the fake network's sockets, one
// per cid. The stack has SIM7000_CA_CIDS connections: TinyGSM indexes its
// socket array by every cid that CARECV? and CASTATE? name.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/runtime.h"

#include "modem.h"
#include "net.h"
#include "sim7000.h"
#include "text.h"

static bool ca_socket(const struct sim7000 *s, int32_t cid) {
    return s->ses.owner[cid] == SIM7000_OWNER_CA && s->sockets[cid].state != NET_SOCK_CLOSED;
}

// A cid of an open or server-closed connection, else CME 3.
static bool socket_cid(struct modem *m, const struct modem_args *a, int32_t *cid) {
    if (!modem_arg_int(a, 0, 0, SIM7000_CA_CIDS - 1, cid)) {
        modem_error(m);
        return false;
    }
    if (!ca_socket(sim7000_of(m), *cid)) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return false;
    }
    return true;
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

// Manual 18.2.3 read form, one line per open connection; the address it
// resolved to stands for the server.
static void caopen_list(struct modem *m) {
    const struct sim7000 *s = sim7000_of(m);
    struct text t;
    bool any = false;

    for (uint8_t cid = 0; cid < SIM7000_CA_CIDS; cid++) {
        const struct net_socket *sock = &s->sockets[cid];
        if (s->ses.owner[cid] != SIM7000_OWNER_CA || sock->state != NET_SOCK_OPEN) {
            continue;
        }
        if (!any) {
            modem_out_str(m, "\r\n");
            any = true;
        }
        text_init(&t);
        text_str(&t, "+CAOPEN: ");
        text_u32(&t, cid);
        text_str(&t, sock->udp ? ",\"UDP\",\"" : ",\"TCP\",\"");
        text_ip(&t, sock->ip);
        text_str(&t, "\",");
        text_u32(&t, sock->port);
        modem_info_line(m, &t);
    }
    modem_ok(m);
}

// Manual 18.2.3, synchronous form: `+CAOPEN: <cid>,<result>`, then OK. The
// connection type is optional (`AT+CAOPEN=<cid>,"host",port`, as TinyGSM
// sends it) and TCP then. TLS set with CASSLCFG is accepted and the same
// plaintext services answer.
static void cmd_caopen(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    int32_t cid;
    int32_t port = 0;
    uint8_t ip[4];

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CAOPEN: (0-1),(\"TCP\",\"UDP\"),64,(1-65535)");
        return;
    }
    if (mode == MODEM_READ) {
        caopen_list(m);
        return;
    }
    if (mode != MODEM_WRITE || a->count < 3 || !modem_arg_int(a, 0, 0, 12, &cid)) {
        modem_error(m);
        return;
    }
    bool typed = a->count >= 4;
    const char *type = typed ? a->v[1].str : "TCP";
    const char *host = a->v[typed ? 2 : 1].str;
    bool udp = text_equal_nocase(type, "UDP");
    uint8_t result = SIM7000_CAOPEN_OK;

    if (cid >= SIM7000_CA_CIDS) {
        result = SIM7000_CAOPEN_CONNECTION_LIMIT;
    } else if (!udp && !text_equal_nocase(type, "TCP")) {
        result = SIM7000_CAOPEN_PARAMETER_INVALID;
    } else if (!sim7000_arg_int(a, typed ? 3 : 2, 1, 65535, &port) || strlen(host) == 0 ||
        strlen(host) > NET_HOST_MAX) {
        result = SIM7000_CAOPEN_PARAMETER_INVALID;
    } else if (s->ses.owner[cid] == SIM7000_OWNER_CIP && s->sockets[cid].state != NET_SOCK_CLOSED) {
        result = SIM7000_CAOPEN_SOCKET_ERROR;
    } else if (s->ses.owner[cid] == SIM7000_OWNER_CA && s->sockets[cid].state == NET_SOCK_OPEN) {
        result = SIM7000_CAOPEN_SOCKET_ERROR;
    } else if ((s->ses.bearers & SIM7000_BEARER_CNACT) == 0) {
        result = SIM7000_CAOPEN_NETWORK_NOT_ACTIVE;
    } else if (!net_resolve(&s->net, host, ip)) {
        result = SIM7000_CAOPEN_DNS_FAILED;
    } else {
        sim7000_forget_socket(s, (uint8_t)cid);
        if (!net_open(&s->net, (uint8_t)cid, udp, ip, (uint16_t)port)) {
            result = SIM7000_CAOPEN_REMOTE_REFUSED;
        } else {
            s->ses.owner[cid] = SIM7000_OWNER_CA;
        }
    }
    modem_trace(m, MCLW_TRACE_COMMAND, (uint32_t)cid, (uint16_t)(result == SIM7000_CAOPEN_OK ? port : 0),
        (int16_t)result);
    caopen_result(m, cid, result);
}

static size_t casend_data(struct modem *m, const uint8_t *bytes, size_t len) {
    net_send(m->net, m->ses.data_arg, bytes, len);
    return len;
}

// Manual 18.2.5: OK first, then `+CASEND: <cid>,<result>,<sendlen>`. TinyGSM
// reads the OK as the end of its wait for `>` and the URC as the result.
static void casend_complete(struct modem *m) {
    struct text t;

    modem_ok(m);
    text_init(&t);
    text_str(&t, "+CASEND: ");
    text_u32(&t, m->ses.data_arg);
    text_str(&t, ",0,");
    text_u32(&t, sim7000_of(m)->ses.sent);
    modem_info(m, &t);
}

// A CASEND data phase ends after <inputtime>; whatever arrived has been sent
// and the command fails.
static void casend_expired(struct modem *m) {
    modem_trace(m, MCLW_TRACE_ERROR, modem_fourcc("CASE"), 0, -ETIMEDOUT);
    modem_error(m);
}

static size_t casend_counted(struct modem *m, const uint8_t *bytes, size_t len) {
    sim7000_of(m)->ses.sent = (uint16_t)(sim7000_of(m)->ses.sent + len);
    return casend_data(m, bytes, len);
}

static const struct modem_phase casend_phase = {
    .data = casend_counted,
    .complete = casend_complete,
    .expired = casend_expired,
};

static void cmd_casend(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;
    int32_t cid;
    int32_t len;
    int32_t input_ms = SIM7000_INPUT_MS;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CASEND: (0-1),(1-1460),(100-10000)");
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!socket_cid(m, a, &cid)) {
        return;
    }
    if (s->sockets[cid].state != NET_SOCK_OPEN) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    if (a->count == 1) {
        text_init(&t);
        text_str(&t, "+CASEND: ");
        text_u32(&t, SIM7000_DATA_MAX);
        modem_answer_text(m, &t);
        return;
    }
    if (!modem_arg_int(a, 1, 1, SIM7000_DATA_MAX, &len) || (a->count > 2 && !modem_arg_int(a, 2, 100, 10000, &input_ms))) {
        modem_error(m);
        return;
    }
    s->ses.sent = 0;
    modem_prompt(m, &casend_phase, (uint16_t)len, (uint8_t)cid, (uint32_t)input_ms);
}

// `+CARECV: <n>,<data>` with no cid in front: TinyGSM parses the first
// number as the length (SIM7000SSL.h:503-504), where the manual (18.2.6)
// shows `<cid>,<recvlen>`. Never with n of 0: a read with nothing buffered
// is an error, as over-reading is (SIM7000SSL.h:526-527).
static void cmd_carecv(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;
    int32_t cid;
    int32_t want;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CARECV: (0-1),(1-1460)");
        return;
    }
    if (mode == MODEM_READ) {
        bool any = false;
        for (uint8_t i = 0; i < SIM7000_CA_CIDS; i++) {
            uint32_t n = net_available(&s->net, i);
            if (!ca_socket(s, i) || n == 0) {
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
    if (n > SIM7000_DATA_MAX) {
        n = SIM7000_DATA_MAX;
    }
    if (n + overhead > room) {
        n = room > overhead ? room - overhead : 0;
    }
    if (n == 0) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    text_init(&t);
    text_str(&t, "\r\n+CARECV: ");
    text_u32(&t, (uint32_t)n);
    text_char(&t, ',');
    modem_out_text(m, &t);
    sim7000_emit(m, (uint8_t)cid, n);
    modem_out_str(m, "\r\n");
    modem_ok(m);
}

// Manual 18.2.8: 1 connected, 0 closed by the server. A server close shows
// only once every byte has been read (the network holds it back), so a
// client that gives up on a closed connection loses nothing.
static void cmd_castate(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    const struct sim7000 *s = sim7000_of(m);
    struct text t;
    bool any = false;

    (void)a;
    if (mode != MODEM_READ) {
        modem_error(m);
        return;
    }
    for (uint8_t i = 0; i < SIM7000_CA_CIDS; i++) {
        if (!ca_socket(s, i)) {
            continue;
        }
        if (!any) {
            modem_out_str(m, "\r\n");
            any = true;
        }
        text_init(&t);
        text_str(&t, "+CASTATE: ");
        text_u32(&t, i);
        text_str(&t, s->sockets[i].state == NET_SOCK_OPEN ? ",1" : ",0");
        modem_info_line(m, &t);
    }
    modem_ok(m);
}

static void cmd_caclose(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t cid;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CACLOSE: (0-1)");
        return;
    }
    if (mode != MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (!socket_cid(m, a, &cid)) {
        return;
    }
    sim7000_forget_socket(sim7000_of(m), (uint8_t)cid);
    modem_ok(m);
}

static void cmd_cacid(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    int32_t cid;

    if (mode == MODEM_READ) {
        modem_answer_u32(m, "+CACID: ", s->ses.ca_cid);
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CACID: (0-1)");
    } else if (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, SIM7000_CA_CIDS - 1, &cid)) {
        s->ses.ca_cid = (uint8_t)cid;
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

// Manual 17.2.1. TinyGSM checks for the `+CSSLCFG:` line of the "ctxindex"
// form; the other forms only configure.
static void cmd_csslcfg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
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

static const struct modem_command commands[] = {
    { "+CACID", cmd_cacid },
    { "+CASSLCFG", modem_cmd_accept },
    { "+CSSLCFG", cmd_csslcfg },
    { "+CAOPEN", cmd_caopen },
    { "+CASEND", cmd_casend },
    { "+CARECV", cmd_carecv },
    { "+CASTATE", cmd_castate },
    { "+CACLOSE", cmd_caclose },
};

const struct modem_commands sim7000_ca_commands = MODEM_COMMANDS(commands);
