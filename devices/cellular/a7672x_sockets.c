// SPDX-License-Identifier: Apache-2.0 OR MIT
// The A7672X's TCP/IP dialect (manual chapter 15): the socket service
// (NETOPEN, NETCLOSE), the CIP* links in buffer access mode, TCP
// keepalive, DNS configuration and lookup, translated to the fake
// network's sockets: link n is socket n, SSL session n socket
// A7672X_LINKS + n.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/runtime.h"

#include "a7672x.h"
#include "modem.h"
#include "net.h"
#include "text.h"

// The piece of a CIPRXGET answer read from a socket per call.
#define READ_CHUNK 64u

void a7672x_net_notify(void *ctx, uint8_t sock, enum net_notice notice) {
    struct a7672x *s = ctx;

    if (!a7672x_sock_free(s, sock)) {
        return;
    }
    if (sock < A7672X_LINKS) {
        modem_event(&s->m, A7672X_NET_DELAY_US, notice == NET_NOTICE_DATA ? A7672X_EV_RXGET : A7672X_EV_IPCLOSE,
            sock);
    } else {
        modem_event(&s->m, A7672X_NET_DELAY_US,
            notice == NET_NOTICE_DATA ? A7672X_EV_CCHEVENT : A7672X_EV_CCH_PEER_CLOSED, (uint8_t)(sock - A7672X_LINKS));
    }
}

// A socket the PPP link's TCP holds reads as a closed link.
static uint8_t link_state(const struct a7672x *s, uint8_t link) {
    return link < A7672X_LINKS && a7672x_sock_free(s, link) ? s->sockets[link].state : NET_SOCK_CLOSED;
}

static bool service_up(const struct a7672x *s) {
    return s->ses.netopen && s->net.ses.bearer;
}

// A connection's URCs that have not gone out yet belong to it alone, not
// to the next one on the same link.
static void forget_link(struct a7672x *s, uint8_t link) {
    static const uint8_t kinds[] = { A7672X_EV_CIPOPEN, A7672X_EV_CIPSEND, A7672X_EV_RXGET, A7672X_EV_IPCLOSE,
        A7672X_EV_CIPCLOSE };

    for (size_t i = 0; i < sizeof(kinds); i++) {
        modem_event_cancel(&s->m, kinds[i], link);
    }
    net_close(&s->net, link);
}

// Manual 15.4: +CIPEVENT, then each link reports itself closed.
void a7672x_drop_links(struct a7672x *s) {
    if (s->ses.netopen) {
        modem_event(&s->m, 0, A7672X_EV_CIPEVENT, 0);
    }
    for (uint8_t link = 0; link < A7672X_LINKS; link++) {
        if (link_state(s, link) == NET_SOCK_CLOSED) {
            continue;
        }
        forget_link(s, link);
        net_abort(&s->net, link);
        modem_event(&s->m, 0, A7672X_EV_IPCLOSE, link);
    }
}

static void ip_error(struct modem *m, const char *info) {
    modem_info_str(m, info);
    modem_error(m);
}

// `<prefix><link>,<err>` and ERROR: the immediate failure form of CIPOPEN
// and CIPCLOSE.
static void link_error(struct modem *m, const char *prefix, int32_t link, uint8_t err) {
    struct text t;

    text_init(&t);
    text_str(&t, prefix);
    text_u32(&t, (uint32_t)link);
    text_char(&t, ',');
    text_u32(&t, err);
    modem_info(m, &t);
    modem_error(m);
}

static void cip_error(struct modem *m, uint8_t err) {
    struct text t;

    text_init(&t);
    text_str(&t, "+CIPERROR: ");
    text_u32(&t, err);
    modem_info(m, &t);
    modem_error(m);
}

// NETOPEN and NETCLOSE take `=<cid>` for context 1 only.
static bool context_form(struct modem *m, enum modem_mode mode, struct modem_args *a, bool *with_cid) {
    int32_t cid;

    *with_cid = mode == MODEM_WRITE;
    if (mode == MODEM_EXEC || (mode == MODEM_WRITE && modem_arg_int(a, 0, 1, 1, &cid))) {
        return true;
    }
    modem_error(m);
    return false;
}

// Manual 15.2.1: OK, then +NETOPEN with the result; a second NETOPEN is
// refused. Context 1 comes up with the service when it was not up already.
static void cmd_netopen(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    bool with_cid;

    if (mode == MODEM_READ) {
        modem_answer(m, s->ses.netopen ? "+NETOPEN: 1" : "+NETOPEN: 0");
        return;
    }
    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (!context_form(m, mode, a, &with_cid)) {
        return;
    }
    if (service_up(s)) {
        ip_error(m, "+IP ERROR: Network is already opened");
        return;
    }
    uint8_t err = a7672x_activate(s) ? A7672X_IP_OK : A7672X_IP_NETWORK_FAILURE;
    s->ses.netopen = err == A7672X_IP_OK;
    modem_ok(m);
    modem_event(m, 0, A7672X_EV_NETOPEN, (uint8_t)(err | (with_cid ? A7672X_WITH_CID : 0)));
}

// Manual 15.2.2: every open link reports +CIPCLOSE before +NETCLOSE. On
// LTE context 1 is the default bearer and stays up.
static void cmd_netclose(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;
    bool with_cid;

    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (!context_form(m, mode, a, &with_cid)) {
        return;
    }
    if (!s->ses.netopen) {
        text_init(&t);
        text_str(&t, with_cid ? "+NETCLOSE: 2,1" : "+NETCLOSE: 2");
        modem_info(m, &t);
        modem_error(m);
        return;
    }
    modem_ok(m);
    for (uint8_t link = 0; link < A7672X_LINKS; link++) {
        if (link_state(s, link) != NET_SOCK_CLOSED) {
            forget_link(s, link);
            modem_event(m, 0, A7672X_EV_CIPCLOSE, link);
        }
    }
    s->ses.netopen = false;
    modem_event(m, 0, A7672X_EV_NETCLOSE, (uint8_t)(A7672X_IP_OK | (with_cid ? A7672X_WITH_CID : 0)));
}

static void cipopen_list(struct modem *m) {
    const struct a7672x *s = a7672x_of(m);
    struct text t;

    modem_out_str(m, "\r\n");
    for (uint8_t link = 0; link <= A7672X_LINK_MAX; link++) {
        text_init(&t);
        text_str(&t, "+CIPOPEN: ");
        text_u32(&t, link);
        if (link_state(s, link) == NET_SOCK_OPEN) {
            const struct net_socket *sock = &s->sockets[link];
            text_str(&t, ",\"TCP\",\"");
            text_ip(&t, sock->ip);
            text_str(&t, "\",");
            text_u32(&t, sock->port);
            text_str(&t, ",-1");
        }
        modem_info_line(m, &t);
    }
    modem_ok(m);
}

// Manual 15.2.3: a TCP client answers OK and then +CIPOPEN with the
// result; refusals before the attempt are +CIPOPEN with ERROR. UDP is not
// modelled.
static void cmd_cipopen(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    int32_t link;
    int32_t port;
    uint8_t ip[4];

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CIPOPEN: (0-9),(\"TCP\",\"UDP\",\"UDP SERVER\")");
        return;
    }
    if (mode == MODEM_READ) {
        cipopen_list(m);
        return;
    }
    if (mode != MODEM_WRITE || a->count < 2 || !modem_arg_int(a, 0, 0, INT32_MAX, &link)) {
        modem_error(m);
        return;
    }
    if (link > A7672X_LINK_MAX) {
        ip_error(m, "+IP ERROR: Invalid parameter");
        return;
    }
    const char *type = a->v[1].str;
    const char *host = a->count > 2 ? a->v[2].str : "";
    size_t host_len = text_strnlen(host, NET_HOST_MAX + 1);

    if (text_equal_nocase(type, "UDP") || text_equal_nocase(type, "UDP SERVER")) {
        link_error(m, "+CIPOPEN: ", link, A7672X_IP_NOT_SUPPORTED);
    } else if (!text_equal_nocase(type, "TCP")) {
        link_error(m, "+CIPOPEN: ", link, A7672X_IP_WRONG_PARAMETER);
    } else if (link >= A7672X_LINKS || !a7672x_sock_free(s, (uint8_t)link)) {
        link_error(m, "+CIPOPEN: ", link, A7672X_IP_CREATE_SOCKET);
    } else if (!service_up(s)) {
        link_error(m, "+CIPOPEN: ", link, A7672X_IP_NOT_OPENED);
    } else if (link_state(s, (uint8_t)link) == NET_SOCK_OPEN) {
        link_error(m, "+CIPOPEN: ", link, A7672X_IP_NOT_SUPPORTED);
    } else if (host_len == 0 || host_len > NET_HOST_MAX || !modem_arg_int(a, 3, 1, 65535, &port)) {
        link_error(m, "+CIPOPEN: ", link, A7672X_IP_WRONG_PARAMETER);
    } else {
        struct a7672x_link *l = &s->ses.links[link];
        uint8_t err = A7672X_IP_OK;
        forget_link(s, (uint8_t)link);
        if (!net_resolve(&s->net, host, ip)) {
            err = A7672X_IP_DNS_FAILED;
        } else if (!net_open(&s->net, (uint8_t)link, false, ip, (uint16_t)port)) {
            err = A7672X_IP_NETWORK_FAILURE;
        }
        memset(l, 0, sizeof(*l));
        l->open_err = err;
        modem_trace(m, MCLW_TRACE_COMMAND, (uint32_t)link, (uint16_t)(err == A7672X_IP_OK ? port : 0),
            (int16_t)err);
        modem_ok(m);
        modem_event(m, 0, A7672X_EV_CIPOPEN, (uint8_t)link);
    }
}

static size_t cipsend_data(struct modem *m, const uint8_t *bytes, size_t len) {
    struct a7672x *s = a7672x_of(m);
    uint8_t link = m->ses.data_arg;

    net_send(&s->net, link, bytes, len);
    s->ses.links[link].sent += (uint32_t)len;
    return len;
}

static void cipsend_complete(struct modem *m) {
    struct a7672x *s = a7672x_of(m);
    uint8_t link = m->ses.data_arg;

    s->ses.links[link].send_lost = link_state(s, link) != NET_SOCK_OPEN;
    modem_ok(m);
    modem_event(m, 0, A7672X_EV_CIPSEND, link);
}

static const struct modem_phase cipsend_phase = {
    .data = cipsend_data,
    .complete = cipsend_complete,
    .expired = NULL,
};

// Manual 15.2.4, the fixed-length TCP form: `>`, the data, OK, then
// +CIPSEND: <link>,<req>,<cnf>. The data phase has no input timeout, as on
// the module. The Ctrl-Z form and the UDP forms are not modelled.
static void cmd_cipsend(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    int32_t link;
    int32_t len;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CIPSEND: (0-9),(1-1500)");
        return;
    }
    if (mode == MODEM_READ) {
        modem_ok(m);
        return;
    }
    // Manual 15.2.4 note: a trailing comma is accepted.
    if (mode != MODEM_WRITE || a->count < 2 || a->count > 3 || (a->count == 3 && a->v[2].str[0] != '\0') ||
        !modem_arg_int(a, 0, 0, A7672X_LINK_MAX, &link) || a->v[1].str[0] == '\0') {
        modem_error(m);
        return;
    }
    if (!service_up(s)) {
        cip_error(m, A7672X_IP_NOT_OPENED);
    } else if (link_state(s, (uint8_t)link) != NET_SOCK_OPEN) {
        cip_error(m, A7672X_IP_NOT_SUPPORTED);
    } else if (!modem_arg_int(a, 1, 1, A7672X_CIPSEND_MAX, &len)) {
        cip_error(m, A7672X_IP_WRONG_PARAMETER);
    } else {
        s->ses.links[link].send_len = (uint16_t)len;
        modem_prompt(m, &cipsend_phase, (uint16_t)len, (uint8_t)link, 0);
    }
}

static const char hex_digits[] = "0123456789ABCDEF";

static void emit(struct modem *m, uint8_t link, size_t n, bool hex) {
    uint8_t buf[READ_CHUNK];
    char out[2 * READ_CHUNK];

    while (n > 0) {
        size_t got = net_read(m->net, link, buf, n < sizeof(buf) ? n : sizeof(buf));
        if (got == 0) {
            return;
        }
        if (hex) {
            for (size_t i = 0; i < got; i++) {
                out[2 * i] = hex_digits[buf[i] >> 4];
                out[2 * i + 1] = hex_digits[buf[i] & 0x0fu];
            }
            modem_out(m, out, 2 * got);
        } else {
            modem_out(m, buf, got);
        }
        n -= got;
    }
}

// Manual 15.2.5 modes 2 and 3: at most what the output FIFO can take.
static void rxget_read(struct modem *m, uint8_t mode, int32_t link, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    bool hex = mode == 3;
    int32_t max = hex ? A7672X_RXGET_HEX_MAX : A7672X_RXGET_ASCII_MAX;
    int32_t want = max;
    struct text t;

    if (a->count > 2 && !modem_arg_int(a, 2, 1, max, &want)) {
        ip_error(m, "+IP ERROR: Invalid parameter");
        return;
    }
    uint32_t avail = link_state(s, (uint8_t)link) == NET_SOCK_OPEN ? net_available(&s->net, (uint8_t)link) : 0;
    if (avail == 0) {
        ip_error(m, "+IP ERROR: No data");
        return;
    }
    size_t overhead = sizeof("\r\n+CIPRXGET: 3,9,1500,65535\r\n") + sizeof("\r\n\r\nOK\r\n");
    size_t room = modem_out_free(m);
    size_t n = avail < (uint32_t)want ? avail : (size_t)want;
    size_t fit = room > overhead ? (room - overhead) / (hex ? 2u : 1u) : 0;
    if (n > fit) {
        n = fit;
    }
    if (n == 0) {
        ip_error(m, "+IP ERROR: TCP busy");
        return;
    }
    text_init(&t);
    text_str(&t, "\r\n+CIPRXGET: ");
    text_u32(&t, mode);
    text_char(&t, ',');
    text_u32(&t, (uint32_t)link);
    text_char(&t, ',');
    text_u32(&t, (uint32_t)n);
    text_char(&t, ',');
    text_u32(&t, (uint32_t)(avail - n));
    text_str(&t, "\r\n");
    modem_out_text(m, &t);
    emit(m, (uint8_t)link, n, hex);
    s->ses.links[link].received += (uint32_t)n;
    modem_out_str(m, "\r\n");
    modem_ok(m);
}

// Manual 15.2.5: mode 1 buffers what arrives and says so once, when the
// buffer goes from empty to not; modes 2 to 4 work only then. Mode 0, the
// direct push, is accepted but not modelled: data waits in the buffer
// without a URC.
static void cmd_ciprxget(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;
    int32_t op;
    int32_t link;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CIPRXGET: (0-4),(0-9),(1-1500)");
        return;
    }
    if (mode == MODEM_READ) {
        modem_answer_u32(m, "+CIPRXGET: ", s->ses.rxget_mode);
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 4, &op)) {
        modem_error(m);
        return;
    }
    if (op <= 1) {
        if (a->count != 1) {
            ip_error(m, "+IP ERROR: Invalid parameter");
            return;
        }
        s->ses.rxget_mode = (uint8_t)op;
        modem_ok(m);
        return;
    }
    if (!modem_arg_int(a, 1, 0, A7672X_LINK_MAX, &link) || (op == 4 && a->count != 2)) {
        ip_error(m, "+IP ERROR: Invalid parameter");
        return;
    }
    if (s->ses.rxget_mode != 1) {
        ip_error(m, "+IP ERROR: Operation not supported");
        return;
    }
    if (op != 4) {
        rxget_read(m, (uint8_t)op, link, a);
        return;
    }
    text_init(&t);
    text_str(&t, "+CIPRXGET: 4,");
    text_u32(&t, (uint32_t)link);
    text_char(&t, ',');
    text_u32(&t, link_state(s, (uint8_t)link) == NET_SOCK_OPEN ? net_available(&s->net, (uint8_t)link) : 0);
    modem_answer_text(m, &t);
}

// Manual 15.2.6: OK, then +CIPCLOSE: <link>,0. A link the server closed
// whose +IPCLOSE has not gone out yet closes the same way.
static void cmd_cipclose(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;
    int32_t link;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CIPCLOSE: (0-9)");
        return;
    }
    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+CIPCLOSE: ");
        for (uint8_t i = 0; i <= A7672X_LINK_MAX; i++) {
            text_str(&t, i == 0 ? "" : ",");
            text_char(&t, link_state(s, i) == NET_SOCK_OPEN ? '1' : '0');
        }
        modem_answer_text(m, &t);
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, A7672X_LINK_MAX, &link)) {
        modem_error(m);
        return;
    }
    if (link_state(s, (uint8_t)link) == NET_SOCK_CLOSED) {
        link_error(m, "+CIPCLOSE: ", link, A7672X_IP_NOT_SUPPORTED);
        return;
    }
    forget_link(s, (uint8_t)link);
    modem_ok(m);
    modem_event(m, 0, A7672X_EV_CIPCLOSE, (uint8_t)link);
}

// Manual 15.2.16. Everything sent counts as acknowledged at once.
static void cmd_cipack(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;
    int32_t link;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CIPACK: (0-9)");
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, A7672X_LINK_MAX, &link)) {
        ip_error(m, "+IP ERROR: Invalid parameter");
        return;
    }
    if (!service_up(s)) {
        ip_error(m, "+IP ERROR: Network not opened");
        return;
    }
    if (link_state(s, (uint8_t)link) != NET_SOCK_OPEN) {
        ip_error(m, "+IP ERROR: Connection is not created");
        return;
    }
    const struct a7672x_link *l = &s->ses.links[link];
    text_init(&t);
    text_str(&t, "+CIPACK: ");
    text_u32(&t, l->sent);
    text_char(&t, ',');
    text_u32(&t, l->sent);
    text_char(&t, ',');
    text_u32(&t, l->received);
    modem_answer_text(m, &t);
}

static void cmd_ipaddr(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;

    (void)a;
    if (mode != MODEM_EXEC) {
        modem_error(m);
        return;
    }
    if (!service_up(s)) {
        ip_error(m, "+IP ERROR: Network not opened");
        return;
    }
    text_init(&t);
    text_str(&t, "+IPADDR: ");
    text_ip(&t, s->net.set.ip);
    modem_answer_text(m, &t);
}

// Transparent mode is not modelled.
static void cmd_cipmode(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t v;

    if (mode == MODEM_READ) {
        modem_answer(m, "+CIPMODE: 0");
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CIPMODE: (0-1)");
    } else if (mode == MODEM_EXEC || (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, 0, &v))) {
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

// Manual 15.2.19. TinyGSM sets it before NETOPEN although the manual asks
// for it after; the module takes it either way.
static void cmd_ctcpka(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    static const int32_t lo[4] = { 0, 1, 1, 1 };
    static const int32_t hi[4] = { 1, 120, 10, 100 };
    struct a7672x *s = a7672x_of(m);
    struct text t;
    int32_t v[4] = { 0, 0, 0, s->ses.tcpka[3] };

    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+CTCPKA: ");
        for (size_t i = 0; i < 4; i++) {
            text_str(&t, i == 0 ? "" : ",");
            text_u32(&t, s->ses.tcpka[i]);
        }
        modem_answer_text(m, &t);
        return;
    }
    if (mode != MODEM_WRITE || a->count < 3 || a->count > 4) {
        modem_error(m);
        return;
    }
    for (uint8_t i = 0; i < a->count; i++) {
        if (!modem_arg_int(a, i, lo[i], hi[i], &v[i])) {
            modem_error(m);
            return;
        }
    }
    for (size_t i = 0; i < 4; i++) {
        s->ses.tcpka[i] = (uint8_t)v[i];
    }
    modem_ok(m);
}

static bool ipv4_literal(const struct net *net, const char *str, uint8_t ip[4]) {
    return str[0] >= '0' && str[0] <= '9' && net_resolve(net, str, ip);
}

// Manual 15.2.20, IPv4 only. The servers are kept, not used: the fake
// network resolves from the hosts region.
static void cmd_cdnscfg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;
    uint8_t dns[2][4];
    int32_t type = 0;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CDNSCFG: (\"Primary DNS\"),(\"Secondary DNS\"),type");
        return;
    }
    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "Primary IPv4 DNS: ");
        text_ip(&t, s->ses.dns[0]);
        text_str(&t, ",Secondary IPv4 DNS: ");
        text_ip(&t, s->ses.dns[1]);
        modem_info(m, &t);
        modem_out_str(m, "Primary IPv6 DNS: ::,Secondary IPv6 DNS: ::\r\n");
        modem_ok(m);
        return;
    }
    memset(dns, 0, sizeof(dns));
    if (mode != MODEM_WRITE || a->count > 3 || !ipv4_literal(&s->net, a->v[0].str, dns[0]) ||
        (a->count > 1 && a->v[1].str[0] != '\0' && !ipv4_literal(&s->net, a->v[1].str, dns[1])) ||
        (a->count > 2 && !modem_arg_int(a, 2, 0, 0, &type))) {
        modem_error(m);
        return;
    }
    memcpy(s->ses.dns, dns, sizeof(dns));
    modem_ok(m);
}

// Manual 15.2.17: the answer comes with the command.
static void cmd_cdnsgip(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;
    uint8_t ip[4];

    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE || a->count != 1 || a->v[0].str[0] == '\0') {
        modem_error(m);
        return;
    }
    const char *host = a->v[0].str;
    text_init(&t);
    if (!s->net.ses.bearer || text_strnlen(host, NET_HOST_MAX + 1) > NET_HOST_MAX || !net_resolve(&s->net, host, ip)) {
        modem_info_str(m, "+CDNSGIP: 0,10");
        modem_error(m);
        return;
    }
    text_str(&t, "+CDNSGIP: 1,\"");
    text_str(&t, host);
    text_str(&t, "\",\"");
    text_ip(&t, ip);
    text_char(&t, '"');
    modem_answer_text(m, &t);
}

void a7672x_socket_event(struct a7672x *s, uint8_t kind, uint8_t arg) {
    struct modem *m = &s->m;
    struct text t;
    uint8_t link = arg;

    text_init(&t);
    switch (kind) {
        case A7672X_EV_NETOPEN:
        case A7672X_EV_NETCLOSE:
            text_str(&t, kind == A7672X_EV_NETOPEN ? "+NETOPEN: " : "+NETCLOSE: ");
            text_u32(&t, arg & 0x7fu);
            if ((arg & A7672X_WITH_CID) != 0) {
                text_str(&t, ",1");
            }
            break;
        case A7672X_EV_CIPEVENT:
            text_str(&t, "+CIPEVENT: NETWORK CLOSED UNEXPECTEDLY");
            break;
        case A7672X_EV_CIPOPEN:
            text_str(&t, "+CIPOPEN: ");
            text_u32(&t, link);
            text_char(&t, ',');
            text_u32(&t, s->ses.links[link].open_err);
            break;
        case A7672X_EV_CIPSEND:
            text_str(&t, "+CIPSEND: ");
            text_u32(&t, link);
            text_char(&t, ',');
            text_u32(&t, s->ses.links[link].send_len);
            text_char(&t, ',');
            if (s->ses.links[link].send_lost) {
                text_str(&t, "-1");
            } else {
                text_u32(&t, s->ses.links[link].send_len);
            }
            break;
        case A7672X_EV_RXGET:
            if (s->ses.rxget_mode != 1 || link_state(s, link) != NET_SOCK_OPEN || net_available(&s->net, link) == 0) {
                return;
            }
            text_str(&t, "+CIPRXGET: 1,");
            text_u32(&t, link);
            break;
        case A7672X_EV_IPCLOSE:
            // The link is free again once it has said so.
            if (link_state(s, link) != NET_SOCK_REMOTE_CLOSED) {
                return;
            }
            forget_link(s, link);
            text_str(&t, "+IPCLOSE: ");
            text_u32(&t, link);
            text_str(&t, ",1");
            break;
        case A7672X_EV_CIPCLOSE:
            text_str(&t, "+CIPCLOSE: ");
            text_u32(&t, link);
            text_str(&t, ",0");
            break;
        default:
            return;
    }
    modem_info(m, &t);
}

static const struct modem_command commands[] = {
    { "+NETOPEN", cmd_netopen },
    { "+NETCLOSE", cmd_netclose },
    { "+CIPOPEN", cmd_cipopen },
    { "+CIPSEND", cmd_cipsend },
    { "+CIPRXGET", cmd_ciprxget },
    { "+CIPCLOSE", cmd_cipclose },
    { "+CIPACK", cmd_cipack },
    { "+IPADDR", cmd_ipaddr },
    { "+CIPMODE", cmd_cipmode },
    { "+CIPHEAD", modem_cmd_accept },
    { "+CIPSRIP", modem_cmd_accept },
    { "+CIPSENDMODE", modem_cmd_accept },
    { "+CIPTIMEOUT", modem_cmd_accept },
    { "+CIPCCFG", modem_cmd_accept },
    { "+CTCPKA", cmd_ctcpka },
    { "+CDNSCFG", cmd_cdnscfg },
    { "+CDNSGIP", cmd_cdnsgip },
};

const struct modem_commands a7672x_socket_commands = MODEM_COMMANDS(commands);
