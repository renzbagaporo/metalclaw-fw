// SPDX-License-Identifier: Apache-2.0 OR MIT
// The SIM7000's CIP stack (manual chapter 8) in multi-IP, manual-receive
// mode, and the connection bookkeeping both its stacks share, translated to
// the fake network's sockets, one per mux.

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

// The piece of an answer read from a socket per call.
#define READ_CHUNK 64u

void sim7000_net_notify(void *ctx, uint8_t sock, enum net_notice notice) {
    struct sim7000 *s = ctx;

    modem_event(&s->m, 0, notice == NET_NOTICE_DATA ? SIM7000_EV_DATA : SIM7000_EV_CLOSED, sock);
}

static void forget_events(struct sim7000 *s, uint8_t slot) {
    modem_event_cancel(&s->m, SIM7000_EV_DATA, slot);
    modem_event_cancel(&s->m, SIM7000_EV_CLOSED, slot);
    modem_event_cancel(&s->m, SIM7000_EV_CONNECT, slot);
}

// A connection's URCs that have not gone out yet belong to it alone, not to
// the next one on the same slot.
void sim7000_forget_socket(struct sim7000 *s, uint8_t slot) {
    forget_events(s, slot);
    net_close(&s->net, slot);
    s->ses.owner[slot] = SIM7000_OWNER_NONE;
}

void sim7000_drop_sockets(struct sim7000 *s, uint8_t owner) {
    for (uint8_t slot = 0; slot < SIM7000_SOCKETS; slot++) {
        uint8_t state = s->sockets[slot].state;
        if (s->ses.owner[slot] == SIM7000_OWNER_NONE || state == NET_SOCK_CLOSED ||
            (owner != SIM7000_OWNER_NONE && s->ses.owner[slot] != owner)) {
            continue;
        }
        forget_events(s, slot);
        net_abort(&s->net, slot);
        if (state == NET_SOCK_OPEN) {
            modem_event(&s->m, 0, SIM7000_EV_CLOSED, slot);
        }
    }
}

void sim7000_cip_shut(struct sim7000 *s) {
    for (uint8_t slot = 0; slot < SIM7000_SOCKETS; slot++) {
        if (s->ses.owner[slot] == SIM7000_OWNER_CIP) {
            sim7000_forget_socket(s, slot);
        }
    }
    s->ses.cip_state = SIM7000_CIP_INITIAL;
    sim7000_bearer(s, SIM7000_BEARER_CIP, false);
}

void sim7000_emit(struct modem *m, uint8_t slot, size_t n) {
    uint8_t buf[READ_CHUNK];

    while (n > 0) {
        size_t got = net_read(m->net, slot, buf, n < sizeof(buf) ? n : sizeof(buf));
        if (got == 0) {
            return;
        }
        modem_out(m, buf, got);
        n -= got;
    }
}

// Manual 8.2.27 `+CIPRXGET: 1,<id>` when readable data appears, 8.2 URC list
// `<n>, CLOSED` when the server closed after everything was read, and
// 8.2.2 `<n>, CONNECT OK`; the CA stack's `+CADATAIND` and `+CASTATE` share
// the kinds, told apart by the slot's owner.
void sim7000_socket_event(struct sim7000 *s, uint8_t kind, uint8_t arg) {
    const struct net_socket *sock = &s->sockets[arg];
    uint8_t owner = s->ses.owner[arg];
    struct text t;

    text_init(&t);
    if (kind == SIM7000_EV_CONNECT) {
        text_u32(&t, arg);
        text_str(&t, s->ses.connect[arg] == SIM7000_CONNECT_OK ? ", CONNECT OK" :
                s->ses.connect[arg] == SIM7000_CONNECT_FAIL ? ", CONNECT FAIL" : ", ALREADY CONNECT");
    } else if (kind == SIM7000_EV_DATA) {
        if (sock->state == NET_SOCK_CLOSED || net_available(&s->net, arg) == 0) {
            return;
        }
        if (owner == SIM7000_OWNER_CA) {
            text_str(&t, "+CADATAIND: ");
            text_u32(&t, arg);
        } else if (owner == SIM7000_OWNER_CIP && s->ses.rxget == 1) {
            text_str(&t, "+CIPRXGET: 1,");
            text_u32(&t, arg);
        } else {
            return;
        }
    } else if (kind == SIM7000_EV_CLOSED) {
        if (sock->state != NET_SOCK_REMOTE_CLOSED) {
            return;
        }
        if (owner == SIM7000_OWNER_CA) {
            text_str(&t, "+CASTATE: ");
            text_u32(&t, arg);
            text_str(&t, ",0");
        } else if (owner == SIM7000_OWNER_CIP) {
            text_u32(&t, arg);
            text_str(&t, ", CLOSED");
        } else {
            return;
        }
    } else {
        return;
    }
    modem_info(&s->m, &t);
}

static bool cip_socket(const struct sim7000 *s, int32_t mux, uint8_t state) {
    return s->ses.owner[mux] == SIM7000_OWNER_CIP && s->sockets[mux].state == state;
}

// Manual 8.2.1: multi-IP mode is switched in IP INITIAL only; single
// connections (+CIPMUX=0) are accepted there but not modelled.
static void cmd_cipmux(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    int32_t n;

    if (mode == MODEM_READ) {
        modem_answer(m, s->ses.cip_mux == 1 ? "+CIPMUX: 1" : "+CIPMUX: 0");
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CIPMUX: (0,1)");
    } else if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 1, &n) || s->ses.cip_state != SIM7000_CIP_INITIAL) {
        modem_error(m);
    } else {
        s->ses.cip_mux = (uint8_t)n;
        modem_ok(m);
    }
}

static void cmd_cipqsend(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    static const uint8_t allowed[] = { 0, 1 };

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CIPQSEND: (0,1)");
    } else {
        modem_small_setting(m, mode, a, "+CIPQSEND: ", &sim7000_of(m)->ses.cip_qsend, allowed, sizeof(allowed));
    }
}

// Manual 8.2.27, as TinyGSM reads it (SIM7000.h:361-408): the answer to mode
// 2 is `+CIPRXGET: 2,<id>,<n>,<rest>` with n the bytes that follow and rest
// the bytes left in the buffer, not the manual's <reqlength>,<cnflength>.
static void cipread(struct modem *m, int32_t mux, int32_t want) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;
    size_t overhead = sizeof("\r\n+CIPRXGET: 2,7,1460,2920\r\n") + sizeof("\r\nOK\r\n");
    size_t room = modem_out_free(m);
    size_t n = net_available(&s->net, (uint8_t)mux);

    if (n > (size_t)want) {
        n = (size_t)want;
    }
    if (n + overhead > room) {
        n = room > overhead ? room - overhead : 0;
    }
    text_init(&t);
    text_str(&t, "+CIPRXGET: 2,");
    text_u32(&t, (uint32_t)mux);
    text_char(&t, ',');
    text_u32(&t, (uint32_t)n);
    text_char(&t, ',');
    text_u32(&t, net_available(&s->net, (uint8_t)mux) - (uint32_t)n);
    modem_info(m, &t);
    sim7000_emit(m, (uint8_t)mux, n);
    modem_ok(m);
}

static void cmd_ciprxget(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;
    int32_t op;
    int32_t mux;
    int32_t want;

    if (mode == MODEM_READ) {
        modem_answer(m, s->ses.rxget == 1 ? "+CIPRXGET: 1" : "+CIPRXGET: 0");
        return;
    }
    if (mode == MODEM_TEST) {
        modem_answer(m, "+CIPRXGET: (1,2,4),(0-7),(1-1460)");
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 4, &op)) {
        modem_error(m);
    } else if (op == 1 && a->count == 1) {
        s->ses.rxget = 1;
        modem_ok(m);
    } else if (op == 2 && modem_arg_int(a, 1, 0, SIM7000_SOCKETS - 1, &mux) &&
        modem_arg_int(a, 2, 1, SIM7000_DATA_MAX, &want)) {
        if (s->ses.cip_mux != 1 || !cip_socket(s, mux, NET_SOCK_OPEN)) {
            modem_cme(m, MODEM_CME_NOT_ALLOWED);
            return;
        }
        cipread(m, mux, want);
    } else if (op == 4 && modem_arg_int(a, 1, 0, SIM7000_SOCKETS - 1, &mux)) {
        text_init(&t);
        text_str(&t, "+CIPRXGET: 4,");
        text_u32(&t, (uint32_t)mux);
        text_char(&t, ',');
        text_u32(&t, s->ses.owner[mux] == SIM7000_OWNER_CIP ? net_available(&s->net, (uint8_t)mux) : 0);
        modem_answer_text(m, &t);
    } else {
        modem_error(m);
    }
}

// Manual 8.2.7: every connection shuts, whatever the state.
static void cmd_cipshut(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (mode == MODEM_EXEC) {
        sim7000_cip_shut(sim7000_of(m));
        modem_info_str(m, "SHUT OK");
    } else {
        modem_error(m);
    }
}

// Manual 8.2.9: IP INITIAL to IP START.
static void cmd_cstt(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CSTT: \"APN\",\"USER\",\"PWD\"");
    } else if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+CSTT: \"");
        text_str(&t, s->ses.apn);
        text_str(&t, "\",\"\",\"\"");
        modem_answer_text(m, &t);
    } else if (s->ses.cip_state != SIM7000_CIP_INITIAL) {
        modem_error(m);
    } else {
        if (mode == MODEM_WRITE && a->count >= 1 && a->v[0].str[0] != '\0') {
            sim7000_set_apn(s, a->v[0].str);
        }
        s->ses.cip_state = SIM7000_CIP_START;
        modem_ok(m);
    }
}

// Manual 8.2.10: IP START to IP GPRSACT, which holds the bearer.
static void cmd_ciicr(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);

    (void)a;
    if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (mode != MODEM_EXEC || s->ses.cip_state != SIM7000_CIP_START || !modem_attached(m)) {
        modem_error(m);
    } else {
        s->ses.cip_state = SIM7000_CIP_GPRSACT;
        sim7000_bearer(s, SIM7000_BEARER_CIP, true);
        modem_ok(m);
    }
}

static bool cifsr(struct modem *m) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;

    if (s->ses.cip_state != SIM7000_CIP_GPRSACT && s->ses.cip_state != SIM7000_CIP_STATUS) {
        modem_error(m);
        return false;
    }
    s->ses.cip_state = SIM7000_CIP_STATUS;
    text_init(&t);
    text_ip(&t, s->net.set.ip);
    modem_info(m, &t);
    return true;
}

// Manual 8.2.11: the bare address, no OK.
static void cmd_cifsr(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (mode != MODEM_EXEC) {
        modem_error(m);
    } else {
        cifsr(m);
    }
}

// TinyGSM sends `AT+CIFSR;E0` so the address ends in an OK: the address, then
// the second command of the line, ATE0, and its OK. This is the one `;`
// concatenation the engine is asked for, and it is taken as one command name.
static void cmd_cifsr_e0(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode != MODEM_EXEC) {
        modem_error(m);
    } else if (cifsr(m)) {
        m->chan->echo = false;
        modem_ok(m);
    }
}

static const char *client_state(const struct sim7000 *s, uint8_t mux) {
    if (cip_socket(s, mux, NET_SOCK_OPEN)) {
        return "CONNECTED";
    }
    return cip_socket(s, mux, NET_SOCK_REMOTE_CLOSED) ? "CLOSED" : "INITIAL";
}

// The fields of manual 8.2.13 after the "+CIPSTATUS: " or "C: " prefix. The
// state is quoted, as TinyGSM matches it (`,"CONNECTED"`).
static void status_fields(struct text *t, const struct sim7000 *s, uint8_t mux) {
    const struct net_socket *sock = &s->sockets[mux];
    bool known = s->ses.owner[mux] == SIM7000_OWNER_CIP && sock->state != NET_SOCK_CLOSED;

    text_u32(t, mux);
    if (known) {
        text_str(t, ",0,\"");
        text_str(t, sock->udp ? "UDP" : "TCP");
        text_str(t, "\",\"");
        text_ip(t, sock->ip);
        text_str(t, "\",\"");
        text_u32(t, sock->port);
        text_str(t, "\",\"");
    } else {
        text_str(t, ",,\"\",\"\",\"\",\"");
    }
    text_str(t, client_state(s, mux));
    text_char(t, '"');
}

static const char *const cip_state_names[] = {
    "IP INITIAL",
    "IP START",
    "IP GPRSACT",
    "IP STATUS",
    "PDP DEACT",
};

// Manual 8.2.13, multi-IP: the query for one connection, or OK, the state
// and a C: line for each connection that exists. While one exists, IP
// STATUS reads as the multi-IP state 5, IP PROCESSING.
static void cmd_cipstatus(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;
    int32_t mux;
    bool any = false;

    if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (s->ses.cip_mux != 1) {
        modem_error(m);
    } else if (mode == MODEM_WRITE) {
        if (!modem_arg_int(a, 0, 0, SIM7000_SOCKETS - 1, &mux)) {
            modem_error(m);
            return;
        }
        text_init(&t);
        text_str(&t, "+CIPSTATUS: ");
        status_fields(&t, s, (uint8_t)mux);
        modem_answer_text(m, &t);
    } else if (mode == MODEM_EXEC) {
        bool open = false;
        for (uint8_t mux_i = 0; mux_i < SIM7000_SOCKETS; mux_i++) {
            open = open || (s->ses.owner[mux_i] == SIM7000_OWNER_CIP && s->sockets[mux_i].state != NET_SOCK_CLOSED);
        }
        modem_ok(m);
        text_init(&t);
        text_str(&t, "STATE: ");
        text_str(&t, open && s->ses.cip_state == SIM7000_CIP_STATUS ? "IP PROCESSING"
                                                                    : cip_state_names[s->ses.cip_state]);
        modem_info(m, &t);
        for (uint8_t mux_i = 0; mux_i < SIM7000_SOCKETS; mux_i++) {
            if (s->ses.owner[mux_i] != SIM7000_OWNER_CIP || s->sockets[mux_i].state == NET_SOCK_CLOSED) {
                continue;
            }
            if (!any) {
                modem_out_str(m, "\r\n");
                any = true;
            }
            text_init(&t);
            text_str(&t, "C: ");
            status_fields(&t, s, mux_i);
            modem_info_line(m, &t);
        }
    } else {
        modem_error(m);
    }
}

// Manual 8.2.2, multi-IP: OK, then `<n>, CONNECT OK` (or FAIL, ALREADY
// CONNECT). Only in IP STATUS. TCP and UDP go to the network's services.
static void cmd_cipstart(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    int32_t mux;
    int32_t port;
    uint8_t ip[4];

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CIPSTART: (0-7),(\"TCP\",\"UDP\"),(IP address),(port)");
        return;
    }
    if (mode != MODEM_WRITE || a->count < 4 || !modem_arg_int(a, 0, 0, SIM7000_SOCKETS - 1, &mux) ||
        !sim7000_arg_int(a, 3, 1, 65535, &port)) {
        modem_error(m);
        return;
    }
    const char *type = a->v[1].str;
    const char *host = a->v[2].str;
    bool udp = text_equal_nocase(type, "UDP");

    if ((!udp && !text_equal_nocase(type, "TCP")) || strlen(host) == 0 || strlen(host) > NET_HOST_MAX ||
        s->ses.cip_mux != 1 || s->ses.cip_state != SIM7000_CIP_STATUS) {
        modem_error(m);
        return;
    }
    uint8_t result = SIM7000_CONNECT_OK;
    uint8_t slot = (uint8_t)mux;
    if (s->ses.owner[slot] == SIM7000_OWNER_CA && s->sockets[slot].state != NET_SOCK_CLOSED) {
        modem_error(m);
        return;
    }
    if (cip_socket(s, mux, NET_SOCK_OPEN)) {
        result = SIM7000_CONNECT_ALREADY;
    } else {
        sim7000_forget_socket(s, slot);
        if (!net_resolve(&s->net, host, ip) || !net_open(&s->net, slot, udp, ip, (uint16_t)port)) {
            result = SIM7000_CONNECT_FAIL;
        } else {
            s->ses.owner[slot] = SIM7000_OWNER_CIP;
        }
    }
    modem_trace(m, MCLW_TRACE_COMMAND, (uint32_t)mux, (uint16_t)(result == SIM7000_CONNECT_OK ? port : 0),
        (int16_t)result);
    s->ses.connect[slot] = result;
    modem_ok(m);
    modem_event(m, 0, SIM7000_EV_CONNECT, slot);
}

static void send_result(struct modem *m, uint8_t mux, uint16_t len) {
    struct text t;

    text_init(&t);
    if (sim7000_of(m)->ses.cip_qsend == 1) {
        text_str(&t, "DATA ACCEPT:");
        text_u32(&t, mux);
        text_char(&t, ',');
        text_u32(&t, len);
    } else {
        text_u32(&t, mux);
        text_str(&t, ", SEND OK");
    }
    modem_info(m, &t);
}

static size_t cipsend_data(struct modem *m, const uint8_t *bytes, size_t len) {
    net_send(m->net, m->ses.data_arg, bytes, len);
    sim7000_of(m)->ses.sent = (uint16_t)(sim7000_of(m)->ses.sent + len);
    return len;
}

// With QSEND 1 the data is accepted as soon as the module has it: the line
// stands for the final result and there is no OK (manual 8.2.3, 8.2.4).
static void cipsend_complete(struct modem *m) {
    send_result(m, m->ses.data_arg, sim7000_of(m)->ses.sent);
}

static void cipsend_expired(struct modem *m) {
    modem_trace(m, MCLW_TRACE_ERROR, modem_fourcc("CIPS"), 0, -ETIMEDOUT);
    modem_error(m);
}

static const struct modem_phase cipsend_phase = {
    .data = cipsend_data,
    .complete = cipsend_complete,
    .expired = cipsend_expired,
};

// The variable-length form ends at Ctrl-Z; ESC cancels.
static size_t cipsend_open_data(struct modem *m, const uint8_t *bytes, size_t len) {
    struct sim7000 *s = sim7000_of(m);
    size_t i = 0;

    while (i < len && bytes[i] != MODEM_CTRL_Z && bytes[i] != MODEM_ESC) {
        i++;
    }
    cipsend_data(m, bytes, i);
    if (i < len) {
        modem_phase_end(m);
        if (bytes[i] == MODEM_CTRL_Z) {
            send_result(m, m->ses.data_arg, s->ses.sent);
        } else {
            modem_ok(m);
        }
        i++;
    }
    return i;
}

static const struct modem_phase cipsend_open_phase = {
    .data = cipsend_open_data,
    .complete = cipsend_complete,
    .expired = cipsend_expired,
};

static void cmd_cipsend(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    int32_t mux;
    int32_t len;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CIPSEND: (0-7),(1-1460)");
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, SIM7000_SOCKETS - 1, &mux) ||
        (a->count > 1 && !modem_arg_int(a, 1, 1, SIM7000_DATA_MAX, &len))) {
        modem_error(m);
        return;
    }
    if (s->ses.cip_mux != 1 || !cip_socket(s, mux, NET_SOCK_OPEN)) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    s->ses.sent = 0;
    if (a->count > 1) {
        modem_prompt(m, &cipsend_phase, (uint16_t)len, (uint8_t)mux, SIM7000_INPUT_MS);
    } else {
        modem_prompt(m, &cipsend_open_phase, 0, (uint8_t)mux, 0);
    }
}

// Manual 8.2.6, multi-IP: `<id>, CLOSE OK`, and ERROR for a connection that
// is not open or closed by the server (which TinyGSM's stop() ignores).
static void cmd_cipclose(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;
    int32_t mux;

    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, SIM7000_SOCKETS - 1, &mux) ||
        s->ses.owner[mux] != SIM7000_OWNER_CIP) {
        modem_error(m);
        return;
    }
    sim7000_forget_socket(s, (uint8_t)mux);
    text_init(&t);
    text_u32(&t, (uint32_t)mux);
    text_str(&t, ", CLOSE OK");
    modem_info(m, &t);
}

static const struct modem_command commands[] = {
    { "+CIPMUX", cmd_cipmux },
    { "+CIPQSEND", cmd_cipqsend },
    { "+CIPRXGET", cmd_ciprxget },
    { "+CIPSHUT", cmd_cipshut },
    { "+CSTT", cmd_cstt },
    { "+CIICR", cmd_ciicr },
    { "+CIFSR", cmd_cifsr },
    { "+CIFSR;E0", cmd_cifsr_e0 },
    { "+CIPSTATUS", cmd_cipstatus },
    { "+CIPSTART", cmd_cipstart },
    { "+CIPSEND", cmd_cipsend },
    { "+CIPCLOSE", cmd_cipclose },
};

const struct modem_commands sim7000_cip_commands = MODEM_COMMANDS(commands);
