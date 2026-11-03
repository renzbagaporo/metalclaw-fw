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

// Zephyr (subsys/net/ip/udp.c) accepts a zero checksum and requires the
// UDP length to match the IP payload.
void udp_input(struct ip *ip, const uint8_t src[4], const uint8_t dst[4], const uint8_t *dgram, size_t len) {
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
    if (dport == NET_PORT_ECHO || dport == NET_PORT_DISCARD) {
        ip->net->stats.bytes_up += (uint32_t)n;
    }
    if (dport == NET_PORT_DISCARD) {
        return;
    }
    if ((dport != NET_PORT_ECHO && dport != UDP_PORT_DNS) || slot->busy) {
        drop(ip, n);
        return;
    }
    if (dport == NET_PORT_ECHO) {
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
