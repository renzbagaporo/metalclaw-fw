// SPDX-License-Identifier: Apache-2.0 OR MIT
// UDP behind the PPP link, by destination port: 53 DNS, 7 echo, 9
// discard; anything else is dropped and counted in the network's `drops`.
// A reply waits in the stack's one slot; a datagram that finds it taken,
// or does not fit, is dropped and counted.

#ifndef MCLW_DEVICES_CELLULAR_UDP_H
#define MCLW_DEVICES_CELLULAR_UDP_H

#include <stddef.h>
#include <stdint.h>

#include "ip.h"

#define UDP_HEADER 8u
#define UDP_PORT_DNS 53u

void udp_input(struct ip *ip, const uint8_t src[4], const uint8_t dst[4], const uint8_t *dgram, size_t len);

#endif
