// SPDX-License-Identifier: Apache-2.0 OR MIT
// The far end of a PPP link as Zephyr's native IPv4 stack sees it: every
// address it sends to is "the internet". IPv4 without options or
// fragments, ICMP echo, UDP (DNS from the hosts region, echo, discard) and
// a TCP responder that drives the same services as the AT sockets
// (net.h). Replies come from the address the DUT targeted. No allocation:
// every packet the link sends is built on demand by ip_output() into the
// link's transmit buffer, so a retransmission regenerates its bytes from
// the service.
//
// Everything runs on the simulation clock and nothing is random: ISNs and
// IP IDs come from counters.

#ifndef MCLW_DEVICES_CELLULAR_IP_H
#define MCLW_DEVICES_CELLULAR_IP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net.h"

#define IP_HEADER 20u
#define IP_PROTO_ICMP 1u
#define IP_PROTO_TCP 6u
#define IP_PROTO_UDP 17u
#define IP_TTL 64u

// The connections the responder holds at once. Each takes a closed socket
// of the model's net while it lives.
#define IP_TCP_CONNS 4u
// One UDP or ICMP reply waiting for the link: a 256-byte echo with room
// to spare, and any DNS answer of a 64-character name.
#define IP_SLOT_MAX 272u

// What the stack observed: kept across power cycles, cleared by reset().
struct ip_stats {
    uint32_t tcp_retransmits;
};

// A reply the stack owes: UDP (DNS, echo) or an ICMP echo reply, its
// payload already built.
struct ip_slot {
    bool busy;
    uint8_t proto;
    uint8_t src[4];
    uint8_t dst[4];
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint8_t data[IP_SLOT_MAX];
};

// A RST the stack owes a segment no connection takes.
struct ip_rst {
    bool busy;
    bool ack;
    uint8_t src[4];
    uint8_t dst[4];
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack_seq;
};

struct tcp_conn {
    uint64_t due_us;
    uint32_t iss;
    // The oldest byte the DUT has not acknowledged, the next to send, the
    // highest sent, and the next the model expects.
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t snd_max;
    uint32_t rcv_nxt;
    uint16_t peer_port;
    uint16_t port;
    uint16_t snd_wnd;
    uint16_t mss;
    uint16_t adv_wnd;
    uint8_t addr[4];
    uint8_t state;
    uint8_t sock;
    uint8_t timer;
    uint8_t tries;
    uint8_t flags;
};

struct ip {
    struct net *net;
    // The largest packet either side sends: the model's MRU.
    uint16_t mtu;
    uint16_t id;
    uint16_t conn_seq;
    uint8_t next;
    struct ip_rst rst;
    struct ip_slot slot;
    struct tcp_conn conns[IP_TCP_CONNS];
    struct ip_stats stats;
};

void ip_init(struct ip *ip, struct net *net);
// The link came up with `mtu`: no connection, nothing owed.
void ip_open(struct ip *ip, uint16_t mtu);
// The link went down: every connection ends without a word, its socket
// closed.
void ip_close(struct ip *ip);
// One IPv4 packet from the DUT.
void ip_input(struct ip *ip, const uint8_t *pkt, size_t len, uint64_t now_us);
// The next packet the stack wants to send, at most `max` bytes, written
// into `out`; 0 when it has none. Control (SYN-ACK, pure ACKs, FIN, RST)
// before replies before data.
size_t ip_output(struct ip *ip, uint8_t *out, size_t max, uint64_t now_us);
void ip_tick(struct ip *ip, uint64_t now_us);
// The socket is a TCP connection of the stack's.
bool ip_owns_socket(const struct ip *ip, uint8_t sock);

// RFC 1071 one's complement sum of `len` bytes added to `sum`, unfolded.
uint32_t ip_sum(uint32_t sum, const uint8_t *p, size_t len);
uint16_t ip_fold(uint32_t sum);
// Writes an IPv4 header for `payload` bytes after it.
void ip_header(struct ip *ip, uint8_t *out, uint8_t proto, const uint8_t src[4], const uint8_t dst[4],
    size_t payload);
// The TCP/UDP checksum of `seg` with its pseudo header.
uint16_t ip_l4_checksum(uint8_t proto, const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, size_t len);

uint16_t ip_get16(const uint8_t *p);
uint32_t ip_get32(const uint8_t *p);
void ip_put16(uint8_t *p, uint16_t v);
void ip_put32(uint8_t *p, uint32_t v);

#endif
