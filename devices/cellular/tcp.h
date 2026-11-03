// SPDX-License-Identifier: Apache-2.0 OR MIT
// The TCP responder behind the PPP link, tuned to Zephyr's TCP
// (subsys/net/ip/tcp.c): a fixed 200-299 ms RTO that never measures the
// RTT, fast retransmit on the third duplicate ACK, MSS from the SYN only.
// Every data segment is acknowledged at once; at most two segments are in
// flight; the model retransmits from the service after its own RTO.

#ifndef MCLW_DEVICES_CELLULAR_TCP_H
#define MCLW_DEVICES_CELLULAR_TCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ip.h"

// The model's RTO, its backoff and its patience (spec D.3).
#define TCP_RTO_US 1000000u
#define TCP_TRIES 6u
// A zero window is probed after this long.
#define TCP_PERSIST_US 2000000u
// How long a closed connection stays to acknowledge a retransmitted FIN.
#define TCP_TIME_WAIT_US 2000000u
// RFC 9293 3.7.1: the MSS a SYN without the option implies.
#define TCP_DEFAULT_MSS 536u
#define TCP_ISN_BASE 0x4d430000u

void tcp_input(struct ip *ip, const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, size_t len,
    uint64_t now_us);
// The next segment of one connection, as a whole IP packet. With `data`
// false only control and pure ACKs; with it true, data too.
size_t tcp_output(struct ip *ip, uint8_t *out, size_t max, bool data, uint64_t now_us);
void tcp_tick(struct ip *ip, uint64_t now_us);
// Every connection ends at once and its socket is closed.
void tcp_close_all(struct ip *ip);

#endif
