// SPDX-License-Identifier: Apache-2.0 OR MIT
// A DNS server (RFC 1035) for the hosts region: one A question per query,
// as Zephyr's resolver sends it, answered from `name=a.b.c.d` lines
// without regard to case. A miss is NXDOMAIN, so the resolver fails at
// once instead of waiting for a timeout.

#ifndef MCLW_DEVICES_CELLULAR_DNS_H
#define MCLW_DEVICES_CELLULAR_DNS_H

#include <stddef.h>
#include <stdint.h>

#include "net.h"

#define DNS_HEADER 12u
#define DNS_TTL_S 60u

// Writes the reply to `query` into `out`; 0 when the query is not one to
// answer (malformed, a response, more than one question).
size_t dns_answer(const struct net *net, const uint8_t *query, size_t len, uint8_t *out, size_t max);

#endif
