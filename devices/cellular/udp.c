// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dns.h"
#include "ip.h"
#include "net.h"
#include "udp.h"

static void drop(struct ip *ip, size_t len) {
    ip->net->stats.drops += (uint32_t)len;
}

// The test endpoint's receiver (te_udp_receive.py): a packet counts as
// received when every byte is the low byte of the C library's LCG seeded
// with 1234, else as dropped; two seconds after a sender's last packet it
// is sent its two counts and forgotten.
#define TE_RECEIVE_QUIET_US 2000000u

static bool te_packet_valid(const uint8_t *payload, size_t n) {
    uint32_t state = 1234;

    for (size_t i = 0; i < n; i++) {
        state = (1103515245u * state + 12345u) & 0x7fffffffu;
        if ((uint8_t)state != payload[i]) {
            return false;
        }
    }
    return true;
}

static void te_receive(struct ip *ip, const uint8_t src[4], const uint8_t dst[4], uint16_t sport,
    const uint8_t *payload, size_t n, uint64_t now_us) {
    struct udp_receive *r = &ip->receive;

    // One sender at a time: another's packets wait for the first's report.
    if (r->active && (memcmp(r->addr, src, 4) != 0 || r->port != sport)) {
        drop(ip, n);
        return;
    }
    if (!r->active) {
        r->active = true;
        memcpy(r->addr, src, 4);
        memcpy(r->local, dst, 4);
        r->port = sport;
        r->received = 0;
        r->dropped = 0;
    }
    if (te_packet_valid(payload, n)) {
        r->received++;
    } else {
        r->dropped++;
    }
    r->last_us = now_us;
}

void udp_tick(struct ip *ip, uint64_t now_us) {
    struct udp_receive *r = &ip->receive;
    struct ip_slot *slot = &ip->slot;

    if (!r->active || now_us - r->last_us < TE_RECEIVE_QUIET_US || slot->busy) {
        return;
    }
    slot->data[0] = r->received;
    slot->data[1] = r->dropped;
    slot->len = 2;
    slot->busy = true;
    slot->proto = IP_PROTO_UDP;
    memcpy(slot->src, r->local, 4);
    memcpy(slot->dst, r->addr, 4);
    slot->sport = NET_PORT_TE_RECEIVE;
    slot->dport = r->port;
    r->active = false;
}

// Zephyr (subsys/net/ip/udp.c) accepts a zero checksum and requires the
// UDP length to match the IP payload.
void udp_input(struct ip *ip, const uint8_t src[4], const uint8_t dst[4], const uint8_t *dgram, size_t len,
    uint64_t now_us) {
    struct ip_slot *slot = &ip->slot;

    if (len < UDP_HEADER || ip_get16(&dgram[4]) != len ||
        (ip_get16(&dgram[6]) != 0 && ip_l4_checksum(IP_PROTO_UDP, src, dst, dgram, len) != 0)) {
        return;
    }
    uint16_t sport = ip_get16(&dgram[0]);
    uint16_t dport = ip_get16(&dgram[2]);
    const uint8_t *payload = &dgram[UDP_HEADER];
    size_t n = len - UDP_HEADER;
    size_t room = ip->mtu > IP_HEADER + UDP_HEADER ? ip->mtu - IP_HEADER - UDP_HEADER : 0;
    if (room > IP_SLOT_MAX) {
        room = IP_SLOT_MAX;
    }
    bool echo = dport == NET_PORT_ECHO || dport == NET_PORT_TE_ECHO;

    if (echo || dport == NET_PORT_DISCARD || dport == NET_PORT_TE_RECEIVE) {
        ip->net->stats.bytes_up += (uint32_t)n;
    }
    if (dport == NET_PORT_DISCARD) {
        return;
    }
    if (dport == NET_PORT_TE_RECEIVE) {
        te_receive(ip, src, dst, sport, payload, n, now_us);
        return;
    }
    if ((!echo && dport != UDP_PORT_DNS) || slot->busy) {
        drop(ip, n);
        return;
    }
    if (echo) {
        if (n > room) {
            drop(ip, n);
            return;
        }
        memcpy(slot->data, payload, n);
        slot->len = (uint16_t)n;
    } else {
        slot->len = (uint16_t)dns_answer(ip->net, payload, n, slot->data, room);
        if (slot->len == 0) {
            return;
        }
    }
    slot->busy = true;
    slot->proto = IP_PROTO_UDP;
    memcpy(slot->src, dst, 4);
    memcpy(slot->dst, src, 4);
    slot->sport = dport;
    slot->dport = sport;
}
