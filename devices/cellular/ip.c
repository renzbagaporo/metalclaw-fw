// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ip.h"
#include "net.h"
#include "tcp.h"
#include "udp.h"

#define ICMP_ECHO_REPLY 0u
#define ICMP_ECHO_REQUEST 8u
#define ICMP_HEADER 8u
// The fragment offset and More Fragments bits of the flags word.
#define IP_FRAGMENT_MASK 0x3fffu

uint16_t ip_get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t ip_get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void ip_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void ip_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint32_t ip_sum(uint32_t sum, const uint8_t *p, size_t len) {
    size_t i = 0;

    for (; i + 1 < len; i += 2) {
        sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    }
    if (i < len) {
        sum += (uint32_t)(p[i] << 8);
    }
    return sum;
}

uint16_t ip_fold(uint32_t sum) {
    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

void ip_header(struct ip *ip, uint8_t *out, uint8_t proto, const uint8_t src[4], const uint8_t dst[4],
    size_t payload) {
    out[0] = 0x45;
    out[1] = 0;
    ip_put16(&out[2], (uint16_t)(IP_HEADER + payload));
    ip_put16(&out[4], ip->id++);
    ip_put16(&out[6], 0);
    out[8] = IP_TTL;
    out[9] = proto;
    ip_put16(&out[10], 0);
    memcpy(&out[12], src, 4);
    memcpy(&out[16], dst, 4);
    ip_put16(&out[10], ip_fold(ip_sum(0, out, IP_HEADER)));
}

uint16_t ip_l4_checksum(uint8_t proto, const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, size_t len) {
    uint8_t pseudo[12];

    memcpy(pseudo, src, 4);
    memcpy(&pseudo[4], dst, 4);
    pseudo[8] = 0;
    pseudo[9] = proto;
    ip_put16(&pseudo[10], (uint16_t)len);
    return ip_fold(ip_sum(ip_sum(0, pseudo, sizeof(pseudo)), seg, len));
}

void ip_init(struct ip *ip, struct net *net) {
    memset(ip, 0, sizeof(*ip));
    ip->net = net;
}

void ip_open(struct ip *ip, uint16_t mtu) {
    ip_close(ip);
    ip->mtu = mtu;
}

void ip_close(struct ip *ip) {
    tcp_close_all(ip);
    ip->slot.busy = false;
    ip->rst.busy = false;
}

bool ip_owns_socket(const struct ip *ip, uint8_t sock) {
    for (size_t i = 0; i < IP_TCP_CONNS; i++) {
        if (ip->conns[i].state != 0 && ip->conns[i].sock == sock) {
            return true;
        }
    }
    return false;
}

static void icmp_input(struct ip *ip, const uint8_t src[4], const uint8_t dst[4], const uint8_t *msg, size_t len) {
    struct ip_slot *slot = &ip->slot;

    if (len < ICMP_HEADER || msg[0] != ICMP_ECHO_REQUEST || msg[1] != 0 || ip_fold(ip_sum(0, msg, len)) != 0) {
        return;
    }
    if (slot->busy || len > IP_SLOT_MAX || IP_HEADER + len > ip->mtu) {
        return;
    }
    slot->busy = true;
    slot->proto = IP_PROTO_ICMP;
    memcpy(slot->src, dst, 4);
    memcpy(slot->dst, src, 4);
    slot->len = (uint16_t)len;
    memcpy(slot->data, msg, len);
    slot->data[0] = ICMP_ECHO_REPLY;
    ip_put16(&slot->data[2], 0);
    ip_put16(&slot->data[2], ip_fold(ip_sum(0, slot->data, len)));
}

// Zephyr checks every checksum on a PPP interface and never fragments
// (NET_IPV4_FRAGMENT is off by default), so a packet that is not whole
// and correct is not from it.
void ip_input(struct ip *ip, const uint8_t *pkt, size_t len, uint64_t now_us) {
    if (len < IP_HEADER || (pkt[0] >> 4) != 4) {
        return;
    }
    size_t ihl = (size_t)(pkt[0] & 0x0fu) * 4u;
    size_t total = ip_get16(&pkt[2]);
    if (ihl < IP_HEADER || total < ihl || total > len || (ip_get16(&pkt[6]) & IP_FRAGMENT_MASK) != 0 ||
        ip_fold(ip_sum(0, pkt, ihl)) != 0) {
        return;
    }
    const uint8_t *src = &pkt[12];
    const uint8_t *dst = &pkt[16];
    switch (pkt[9]) {
        case IP_PROTO_TCP:
            tcp_input(ip, src, dst, &pkt[ihl], total - ihl, now_us);
            break;
        case IP_PROTO_UDP:
            udp_input(ip, src, dst, &pkt[ihl], total - ihl);
            break;
        case IP_PROTO_ICMP:
            icmp_input(ip, src, dst, &pkt[ihl], total - ihl);
            break;
        default:
            break;
    }
}

static size_t slot_output(struct ip *ip, uint8_t *out, size_t max) {
    struct ip_slot *slot = &ip->slot;
    size_t l4 = slot->proto == IP_PROTO_UDP ? UDP_HEADER + slot->len : slot->len;

    if (!slot->busy || IP_HEADER + l4 > max) {
        return 0;
    }
    uint8_t *seg = &out[IP_HEADER];
    if (slot->proto == IP_PROTO_UDP) {
        ip_put16(&seg[0], slot->sport);
        ip_put16(&seg[2], slot->dport);
        ip_put16(&seg[4], (uint16_t)l4);
        ip_put16(&seg[6], 0);
        memcpy(&seg[UDP_HEADER], slot->data, slot->len);
        uint16_t sum = ip_l4_checksum(IP_PROTO_UDP, slot->src, slot->dst, seg, l4);
        // RFC 768: a computed zero is sent as all ones.
        ip_put16(&seg[6], sum == 0 ? 0xffffu : sum);
        if (slot->sport == NET_PORT_ECHO) {
            ip->net->stats.bytes_down += slot->len;
        }
    } else {
        memcpy(seg, slot->data, slot->len);
    }
    ip_header(ip, out, slot->proto, slot->src, slot->dst, l4);
    slot->busy = false;
    return IP_HEADER + l4;
}

size_t ip_output(struct ip *ip, uint8_t *out, size_t max, uint64_t now_us) {
    size_t n = tcp_output(ip, out, max, false, now_us);

    if (n == 0) {
        n = slot_output(ip, out, max);
    }
    if (n == 0) {
        n = tcp_output(ip, out, max, true, now_us);
    }
    return n;
}

void ip_tick(struct ip *ip, uint64_t now_us) {
    tcp_tick(ip, now_us);
}
