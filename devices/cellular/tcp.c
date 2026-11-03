// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ip.h"
#include "net.h"
#include "tcp.h"

#define TCP_HEADER 20u
#define TCP_MSS_OPTION 4u

#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

enum {
    TCP_FREE,
    TCP_SYN_RCVD,
    TCP_OPEN,
    TCP_TIME_WAIT,
};

enum {
    T_NONE,
    T_RTO,
    T_PERSIST,
    T_WAIT,
};

// A pure ACK is owed; a SYN-ACK is owed; the DUT's FIN arrived; the
// model's FIN went out; the DUT acknowledged it; the DUT closed first; a
// window update is owed; a zero-window probe is owed.
#define F_ACK 0x01u
#define F_SYNACK 0x02u
#define F_FIN_RCVD 0x04u
#define F_FIN_SENT 0x08u
#define F_FIN_ACKED 0x10u
#define F_PASSIVE 0x20u
#define F_WINUPD 0x40u
#define F_PROBE 0x80u

static bool seq_lt(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static bool seq_le(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) <= 0;
}

static void conn_free(struct ip *ip, struct tcp_conn *c) {
    net_close(ip->net, c->sock);
    memset(c, 0, sizeof(*c));
}

void tcp_close_all(struct ip *ip) {
    for (size_t i = 0; i < IP_TCP_CONNS; i++) {
        if (ip->conns[i].state != TCP_FREE) {
            conn_free(ip, &ip->conns[i]);
        }
    }
}

// The receive window: what the service takes, at most two segments.
static uint16_t window(const struct ip *ip, const struct tcp_conn *c) {
    uint32_t w = net_space(ip->net, c->sock);
    uint32_t cap = 2u * c->mss;

    return (uint16_t)(w < cap ? w : cap);
}

static uint32_t outstanding(const struct tcp_conn *c) {
    return c->snd_nxt - c->snd_una;
}

// Bytes the service has that have not been sent.
static uint32_t unsent(const struct ip *ip, const struct tcp_conn *c) {
    uint32_t avail = net_available(ip->net, c->sock);
    uint32_t sent = outstanding(c);

    return avail > sent ? avail - sent : 0;
}

static uint32_t send_limit(const struct tcp_conn *c) {
    uint32_t cap = 2u * c->mss;

    return c->snd_wnd < cap ? c->snd_wnd : cap;
}

static bool can_send_data(const struct ip *ip, const struct tcp_conn *c) {
    return c->state == TCP_OPEN && (c->flags & (F_FIN_RCVD | F_FIN_SENT)) == 0 && unsent(ip, c) > 0 &&
        outstanding(c) < send_limit(c);
}

static void arm(struct tcp_conn *c, uint8_t timer, uint64_t due_us) {
    c->timer = timer;
    c->due_us = due_us;
}

// One segment as a whole IP packet; `len` payload bytes come from the
// service at `seq`.
static size_t segment(struct ip *ip, struct tcp_conn *c, uint8_t *out, uint8_t flags, uint32_t seq, size_t len) {
    bool mss = (flags & TCP_SYN) != 0;
    size_t header = TCP_HEADER + (mss ? TCP_MSS_OPTION : 0u);
    uint8_t *seg = &out[IP_HEADER];

    ip_put16(&seg[0], c->port);
    ip_put16(&seg[2], c->peer_port);
    ip_put32(&seg[4], seq);
    ip_put32(&seg[8], c->rcv_nxt);
    seg[12] = (uint8_t)((header / 4u) << 4);
    seg[13] = flags;
    c->adv_wnd = window(ip, c);
    ip_put16(&seg[14], c->adv_wnd);
    ip_put16(&seg[16], 0);
    ip_put16(&seg[18], 0);
    if (mss) {
        seg[20] = 2;
        seg[21] = TCP_MSS_OPTION;
        ip_put16(&seg[22], c->mss);
    }
    if (len > 0) {
        net_peek_bytes(ip->net, c->sock, seq - c->snd_una, &seg[header], len);
    }
    ip_put16(&seg[16], ip_l4_checksum(IP_PROTO_TCP, c->addr, ip->net->set.ip, seg, header + len));
    ip_header(ip, out, IP_PROTO_TCP, c->addr, ip->net->set.ip, header + len);
    c->flags = (uint8_t)(c->flags & ~(F_ACK | F_WINUPD | F_PROBE));
    return IP_HEADER + header + len;
}

static size_t rst_output(struct ip *ip, uint8_t *out) {
    struct ip_rst *r = &ip->rst;
    uint8_t *seg = &out[IP_HEADER];

    ip_put16(&seg[0], r->sport);
    ip_put16(&seg[2], r->dport);
    ip_put32(&seg[4], r->seq);
    ip_put32(&seg[8], r->ack ? r->ack_seq : 0);
    seg[12] = (uint8_t)((TCP_HEADER / 4u) << 4);
    seg[13] = (uint8_t)(TCP_RST | (r->ack ? TCP_ACK : 0u));
    ip_put16(&seg[14], 0);
    ip_put16(&seg[16], 0);
    ip_put16(&seg[18], 0);
    ip_put16(&seg[16], ip_l4_checksum(IP_PROTO_TCP, r->src, r->dst, seg, TCP_HEADER));
    ip_header(ip, out, IP_PROTO_TCP, r->src, r->dst, TCP_HEADER);
    r->busy = false;
    return IP_HEADER + TCP_HEADER;
}

// RFC 9293 3.10.7.1: a segment no connection takes. A refused SYN gets
// RST|ACK, which Zephyr reports as ECONNREFUSED.
static void owe_rst(struct ip *ip, const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, size_t dlen) {
    struct ip_rst *r = &ip->rst;
    uint8_t flags = seg[13];

    if (r->busy) {
        return;
    }
    r->busy = true;
    memcpy(r->src, dst, 4);
    memcpy(r->dst, src, 4);
    r->sport = ip_get16(&seg[2]);
    r->dport = ip_get16(&seg[0]);
    if ((flags & TCP_ACK) != 0) {
        r->ack = false;
        r->seq = ip_get32(&seg[8]);
    } else {
        r->ack = true;
        r->seq = 0;
        r->ack_seq = ip_get32(&seg[4]) + (uint32_t)dlen + ((flags & TCP_SYN) != 0 ? 1u : 0u) +
            ((flags & TCP_FIN) != 0 ? 1u : 0u);
    }
}

static uint16_t peer_mss(const uint8_t *seg, size_t doff) {
    size_t p = TCP_HEADER;

    while (p < doff) {
        uint8_t kind = seg[p];
        if (kind == 0) {
            break;
        }
        if (kind == 1) {
            p++;
            continue;
        }
        if (p + 1 >= doff || seg[p + 1] < 2 || p + seg[p + 1] > doff) {
            break;
        }
        if (kind == 2 && seg[p + 1] == TCP_MSS_OPTION) {
            return ip_get16(&seg[p + 2]);
        }
        p += seg[p + 1];
    }
    return TCP_DEFAULT_MSS;
}

static struct tcp_conn *find(struct ip *ip, const uint8_t dst[4], uint16_t sport, uint16_t dport) {
    for (size_t i = 0; i < IP_TCP_CONNS; i++) {
        struct tcp_conn *c = &ip->conns[i];
        if (c->state != TCP_FREE && c->peer_port == sport && c->port == dport && memcmp(c->addr, dst, 4) == 0) {
            return c;
        }
    }
    return NULL;
}

// A free connection and a closed socket, or NULL; a connection in
// TIME_WAIT gives way.
static struct tcp_conn *allocate(struct ip *ip, uint8_t *sock) {
    struct tcp_conn *c = NULL;

    for (size_t i = 0; i < IP_TCP_CONNS && c == NULL; i++) {
        if (ip->conns[i].state == TCP_FREE) {
            c = &ip->conns[i];
        }
    }
    for (size_t i = 0; i < IP_TCP_CONNS && c == NULL; i++) {
        if (ip->conns[i].state == TCP_TIME_WAIT) {
            c = &ip->conns[i];
            conn_free(ip, c);
        }
    }
    if (c == NULL) {
        return NULL;
    }
    for (uint8_t s = 0; s < ip->net->socket_count; s++) {
        if (ip->net->sockets[s].state == NET_SOCK_CLOSED && !ip_owns_socket(ip, s)) {
            *sock = s;
            return c;
        }
    }
    return NULL;
}

static void syn(struct ip *ip, const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, size_t doff) {
    uint16_t sport = ip_get16(&seg[0]);
    uint16_t dport = ip_get16(&seg[2]);
    uint8_t sock = 0;
    struct tcp_conn *c = allocate(ip, &sock);

    // Without a connection to give, the SYN goes unanswered and the DUT
    // retries; a refused port is answered at once.
    if (c == NULL) {
        return;
    }
    if (!net_open(ip->net, sock, false, dst, dport)) {
        owe_rst(ip, src, dst, seg, 0);
        return;
    }
    uint16_t own = (uint16_t)(ip->mtu - IP_HEADER - TCP_HEADER);
    uint16_t theirs = peer_mss(seg, doff);
    c->state = TCP_SYN_RCVD;
    c->sock = sock;
    c->peer_port = sport;
    c->port = dport;
    memcpy(c->addr, dst, 4);
    c->iss = TCP_ISN_BASE + ((uint32_t)ip->conn_seq++ << 16);
    c->snd_una = c->iss + 1u;
    c->snd_nxt = c->snd_una;
    c->snd_max = c->snd_una;
    c->rcv_nxt = ip_get32(&seg[4]) + 1u;
    c->snd_wnd = ip_get16(&seg[14]);
    c->mss = theirs < own ? theirs : own;
    c->flags = F_SYNACK;
}

static void acknowledged(struct ip *ip, struct tcp_conn *c, uint32_t ack, uint64_t now_us) {
    uint32_t acked = ack - c->snd_una;

    if ((c->flags & F_FIN_SENT) != 0 && ack == c->snd_max) {
        c->flags |= F_FIN_ACKED;
        acked--;
    }
    if (acked > 0) {
        bool short_window = c->adv_wnd < c->mss;
        net_discard(ip->net, c->sock, acked);
        if (short_window && window(ip, c) >= c->mss) {
            c->flags |= F_WINUPD;
        }
    }
    c->snd_una = ack;
    if (seq_lt(c->snd_nxt, ack)) {
        c->snd_nxt = ack;
    }
    c->tries = 0;
    if (c->snd_una != c->snd_max) {
        arm(c, T_RTO, now_us + TCP_RTO_US);
    } else if (c->timer == T_RTO) {
        arm(c, T_NONE, 0);
    }
}

static void receive(struct ip *ip, struct tcp_conn *c, uint32_t seq, const uint8_t *data, size_t dlen) {
    if (dlen == 0) {
        return;
    }
    c->flags |= F_ACK;
    if ((c->flags & F_FIN_RCVD) != 0) {
        return;
    }
    int32_t off = (int32_t)(c->rcv_nxt - seq);
    if (off < 0 || (size_t)off >= dlen) {
        return;
    }
    size_t take = dlen - (size_t)off;
    uint32_t space = net_space(ip->net, c->sock);
    if (take > space) {
        take = space;
    }
    net_send(ip->net, c->sock, &data[off], take);
    c->rcv_nxt += (uint32_t)take;
}

void tcp_input(struct ip *ip, const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, size_t len,
    uint64_t now_us) {
    if (len < TCP_HEADER || ip_l4_checksum(IP_PROTO_TCP, src, dst, seg, len) != 0) {
        return;
    }
    size_t doff = (size_t)(seg[12] >> 4) * 4u;
    if (doff < TCP_HEADER || doff > len) {
        return;
    }
    uint16_t sport = ip_get16(&seg[0]);
    uint16_t dport = ip_get16(&seg[2]);
    uint32_t seq = ip_get32(&seg[4]);
    uint32_t ack = ip_get32(&seg[8]);
    uint8_t flags = seg[13];
    size_t dlen = len - doff;
    struct tcp_conn *c = find(ip, dst, sport, dport);

    if ((flags & TCP_RST) != 0) {
        if (c != NULL) {
            conn_free(ip, c);
        }
        return;
    }
    if (c != NULL && (flags & TCP_SYN) != 0) {
        if (c->state == TCP_SYN_RCVD && seq + 1u == c->rcv_nxt) {
            c->flags |= F_SYNACK;
            return;
        }
        if (c->state != TCP_TIME_WAIT) {
            c->flags |= F_ACK;
            return;
        }
        conn_free(ip, c);
        c = NULL;
    }
    if (c == NULL) {
        if ((flags & (TCP_SYN | TCP_ACK)) == TCP_SYN) {
            syn(ip, src, dst, seg, doff);
        } else {
            owe_rst(ip, src, dst, seg, dlen);
        }
        return;
    }
    if (c->state == TCP_TIME_WAIT) {
        if ((flags & TCP_FIN) != 0) {
            c->flags |= F_ACK;
        }
        return;
    }
    if ((flags & TCP_ACK) == 0) {
        return;
    }
    if (c->state == TCP_SYN_RCVD) {
        if (ack != c->snd_una) {
            return;
        }
        c->state = TCP_OPEN;
    }
    if (seq_lt(c->snd_una, ack) && seq_le(ack, c->snd_max)) {
        acknowledged(ip, c, ack, now_us);
    }
    if (seq_le(c->snd_una, ack)) {
        c->snd_wnd = ip_get16(&seg[14]);
        if (c->snd_wnd > 0 && c->timer == T_PERSIST) {
            arm(c, T_NONE, 0);
        }
    }
    receive(ip, c, seq, &seg[doff], dlen);
    if ((flags & TCP_FIN) != 0) {
        uint32_t fin = seq + (uint32_t)dlen;
        if ((c->flags & F_FIN_RCVD) == 0 && fin == c->rcv_nxt) {
            c->rcv_nxt++;
            c->flags |= F_FIN_RCVD | F_ACK;
            if ((c->flags & F_FIN_SENT) == 0) {
                c->flags |= F_PASSIVE;
            }
        } else {
            c->flags |= F_ACK;
        }
    }
    if ((c->flags & (F_FIN_RCVD | F_FIN_ACKED)) == (F_FIN_RCVD | F_FIN_ACKED)) {
        if ((c->flags & F_PASSIVE) != 0) {
            conn_free(ip, c);
        } else {
            c->state = TCP_TIME_WAIT;
            arm(c, T_WAIT, now_us + TCP_TIME_WAIT_US);
        }
    }
}

// The model closes once the DUT has closed, or once the service has
// closed and the DUT has acknowledged everything.
static bool fin_due(const struct ip *ip, const struct tcp_conn *c) {
    if (c->state != TCP_OPEN || (c->flags & F_FIN_SENT) != 0) {
        return false;
    }
    if ((c->flags & F_FIN_RCVD) != 0) {
        return true;
    }
    return ip->net->sockets[c->sock].state == NET_SOCK_REMOTE_CLOSED && c->snd_una == c->snd_max;
}

static size_t control(struct ip *ip, struct tcp_conn *c, uint8_t *out, uint64_t now_us) {
    if ((c->flags & F_SYNACK) != 0) {
        c->flags = (uint8_t)(c->flags & ~F_SYNACK);
        return segment(ip, c, out, TCP_SYN | TCP_ACK, c->iss, 0);
    }
    if (c->state == TCP_SYN_RCVD) {
        return 0;
    }
    if (fin_due(ip, c)) {
        uint32_t seq = c->snd_max;
        c->flags |= F_FIN_SENT;
        c->snd_max++;
        c->snd_nxt = c->snd_max;
        if (c->timer != T_RTO) {
            arm(c, T_RTO, now_us + TCP_RTO_US);
        }
        return segment(ip, c, out, TCP_FIN | TCP_ACK, seq, 0);
    }
    if ((c->flags & F_PROBE) != 0) {
        return segment(ip, c, out, TCP_ACK, c->snd_nxt - 1u, 0);
    }
    // A pure ACK waits when data can carry it.
    if ((c->flags & (F_ACK | F_WINUPD)) != 0 && !can_send_data(ip, c)) {
        return segment(ip, c, out, TCP_ACK, c->snd_nxt, 0);
    }
    return 0;
}

static size_t data(struct ip *ip, struct tcp_conn *c, uint8_t *out, size_t max, uint64_t now_us) {
    if (!can_send_data(ip, c)) {
        if (c->state == TCP_OPEN && c->snd_wnd == 0 && unsent(ip, c) > 0 && outstanding(c) == 0 &&
            c->timer == T_NONE) {
            arm(c, T_PERSIST, now_us + TCP_PERSIST_US);
        }
        return 0;
    }
    uint32_t n = unsent(ip, c);
    uint32_t room = send_limit(c) - outstanding(c);
    if (n > room) {
        n = room;
    }
    if (n > c->mss) {
        n = c->mss;
    }
    if (n + IP_HEADER + TCP_HEADER > max) {
        n = max > IP_HEADER + TCP_HEADER ? (uint32_t)(max - IP_HEADER - TCP_HEADER) : 0;
    }
    if (n == 0) {
        return 0;
    }
    uint32_t seq = c->snd_nxt;
    c->snd_nxt += n;
    if (seq_lt(c->snd_max, c->snd_nxt)) {
        c->snd_max = c->snd_nxt;
    }
    if (c->timer != T_RTO) {
        arm(c, T_RTO, now_us + TCP_RTO_US);
    }
    return segment(ip, c, out, TCP_PSH | TCP_ACK, seq, n);
}

size_t tcp_output(struct ip *ip, uint8_t *out, size_t max, bool with_data, uint64_t now_us) {
    if (max < IP_HEADER + TCP_HEADER + TCP_MSS_OPTION) {
        return 0;
    }
    if (!with_data && ip->rst.busy) {
        return rst_output(ip, out);
    }
    for (size_t k = 0; k < IP_TCP_CONNS; k++) {
        size_t i = (ip->next + k) % IP_TCP_CONNS;
        struct tcp_conn *c = &ip->conns[i];
        if (c->state == TCP_FREE) {
            continue;
        }
        size_t n = with_data ? data(ip, c, out, max, now_us) : control(ip, c, out, now_us);
        if (n > 0) {
            ip->next = (uint8_t)((i + 1) % IP_TCP_CONNS);
            return n;
        }
    }
    return 0;
}

static void expire(struct ip *ip, struct tcp_conn *c, uint64_t now_us) {
    switch (c->timer) {
        case T_RTO:
            if (c->snd_una == c->snd_max) {
                arm(c, T_NONE, 0);
                return;
            }
            if (++c->tries > TCP_TRIES) {
                struct ip_rst *r = &ip->rst;
                if (!r->busy) {
                    r->busy = true;
                    r->ack = true;
                    memcpy(r->src, c->addr, 4);
                    memcpy(r->dst, ip->net->set.ip, 4);
                    r->sport = c->port;
                    r->dport = c->peer_port;
                    r->seq = c->snd_nxt;
                    r->ack_seq = c->rcv_nxt;
                }
                conn_free(ip, c);
                return;
            }
            // Go back to the oldest unacknowledged byte; the FIN, if it
            // was out, follows the data again.
            c->snd_nxt = c->snd_una;
            c->snd_max = c->snd_una;
            c->flags = (uint8_t)(c->flags & ~F_FIN_SENT);
            ip->stats.tcp_retransmits++;
            arm(c, T_RTO, now_us + ((uint64_t)TCP_RTO_US << c->tries));
            return;
        case T_PERSIST:
            c->flags |= F_PROBE;
            arm(c, T_PERSIST, now_us + TCP_PERSIST_US);
            return;
        case T_WAIT:
            conn_free(ip, c);
            return;
        default:
            return;
    }
}

void tcp_tick(struct ip *ip, uint64_t now_us) {
    for (size_t i = 0; i < IP_TCP_CONNS; i++) {
        struct tcp_conn *c = &ip->conns[i];
        if (c->state != TCP_FREE && c->timer != T_NONE && now_us >= c->due_us) {
            expire(ip, c, now_us);
        }
    }
}
