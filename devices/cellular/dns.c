// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dns.h"
#include "net.h"

#define DNS_FLAG_QR 0x80u
#define DNS_OPCODE_MASK 0x78u
#define DNS_TYPE_A 1u
#define DNS_CLASS_IN 1u
// The answer: a pointer to the question's name, type, class, TTL, length,
// the address.
#define DNS_ANSWER 16u
// RFC 1035 4.1.1 RCODE.
#define DNS_RCODE_NXDOMAIN 3u

size_t dns_answer(const struct net *net, const uint8_t *query, size_t len, uint8_t *out, size_t max) {
    char name[NET_HOST_MAX + 1];
    size_t name_len = 0;
    size_t p = DNS_HEADER;
    uint8_t ip[4];

    if (len < DNS_HEADER || (query[2] & (DNS_FLAG_QR | DNS_OPCODE_MASK)) != 0 || query[4] != 0 || query[5] != 1) {
        return 0;
    }
    while (p < len && query[p] != 0) {
        size_t label = query[p];
        // A compression pointer never starts a question's name.
        if (label > 63u || p + 1u + label >= len || name_len + label + 1u > NET_HOST_MAX) {
            return 0;
        }
        if (name_len > 0) {
            name[name_len++] = '.';
        }
        memcpy(&name[name_len], &query[p + 1u], label);
        name_len += label;
        p += 1u + label;
    }
    name[name_len] = '\0';
    // The root label, then QTYPE and QCLASS.
    size_t question_end = p + 5u;
    if (p >= len || question_end > len || name_len == 0) {
        return 0;
    }
    uint16_t qtype = (uint16_t)((query[p + 1u] << 8) | query[p + 2u]);
    uint16_t qclass = (uint16_t)((query[p + 3u] << 8) | query[p + 4u]);
    bool known = net_resolve(net, name, ip);
    bool answer = known && qtype == DNS_TYPE_A && qclass == DNS_CLASS_IN;
    size_t n = question_end + (answer ? DNS_ANSWER : 0u);
    if (n > max) {
        return 0;
    }
    memcpy(out, query, question_end);
    // QR, the query's RD, RA; never AD, CD or Z (Zephyr's dns_pack.h).
    out[2] = (uint8_t)(DNS_FLAG_QR | (query[2] & 0x01u));
    out[3] = (uint8_t)(0x80u | (known ? 0u : DNS_RCODE_NXDOMAIN));
    out[6] = 0;
    out[7] = answer ? 1 : 0;
    memset(&out[8], 0, 4);
    if (answer) {
        static const uint8_t rr[DNS_ANSWER - 4u] = {
            0xc0, DNS_HEADER, 0, DNS_TYPE_A, 0, DNS_CLASS_IN, 0, 0, 0, DNS_TTL_S, 0, 4,
        };
        memcpy(&out[question_end], rr, sizeof(rr));
        memcpy(&out[question_end + sizeof(rr)], ip, 4);
    }
    return n;
}
