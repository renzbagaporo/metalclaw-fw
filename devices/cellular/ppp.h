// SPDX-License-Identifier: Apache-2.0 OR MIT
// PPP as Zephyr's modem_ppp and net/l2/ppp speak it: RFC 1662 HDLC-like
// framing with the FCS-16 (Zephyr never checks the FCS it receives, the
// model sends it right anyway), and minimal LCP (RFC 1661) and IPCP
// (RFC 1332, RFC 1877 DNS) responders that hand Zephyr the address and the
// DNS server it is to use. The link is passive: it sends nothing until the
// DUT's first LCP packet, because Zephyr drops a Configure-Request that
// arrives before its own LCP has started.
//
// The receiver checks the FCS and drops a bad frame. The transmitter
// builds one frame at a time, a control packet before an IP packet, and
// escapes it into whatever room the caller has.

#ifndef MCLW_DEVICES_CELLULAR_PPP_H
#define MCLW_DEVICES_CELLULAR_PPP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ip.h"

#define PPP_MRU_MIN 128u
#define PPP_MRU_MAX 576u
// Small enough that a full segment crosses 115200 baud in about 29 ms,
// so the model's ACKs stay far inside Zephyr's fixed 200 ms RTO.
#define PPP_MRU_DEFAULT 296u

#define PPP_PROTO_IP 0x0021u
#define PPP_PROTO_IPCP 0x8021u
#define PPP_PROTO_LCP 0xc021u

// Restart timer and patience (RFC 1661 4.6), Zephyr's own values.
#define PPP_RESTART_US 3000000u
#define PPP_CONFIGURE_TRIES 10u
#define PPP_TERMINATE_TRIES 2u

// Control packets waiting to go: protocol, then the packet.
#define PPP_CTL_SIZE 96u
// Address, control, protocol and FCS around the information field.
#define PPP_FRAME_OVERHEAD 6u

// Raised for the engine, which clears them.
#define PPP_EV_UP 0x01u
#define PPP_EV_DOWN 0x02u

enum ppp_phase {
    PPP_DEAD,
    PPP_ESTABLISH,
    PPP_NETWORK,
    PPP_TERMINATE,
};

// What the user sets: kept by reset(), restored by reset(factory).
struct ppp_settings {
    uint16_t mru;
    // The model's own address (IPCP 3 of its request) and the DNS server
    // it hands out (RFC 1877 129). The DUT's address is the network's
    // `ip`.
    uint8_t gw[4];
    uint8_t dns[4];
};

// What the link observed: kept across power cycles, cleared by reset().
struct ppp_stats {
    uint32_t bad_fcs;
    uint32_t terminates;
    uint32_t drops;
};

struct ppp_fsm {
    uint64_t due_us;
    uint8_t id;
    uint8_t tries;
    bool requested;
    bool ack_sent;
    bool ack_rcvd;
};

struct ppp {
    const struct ppp_settings *set;
    struct ip *ip;
    uint8_t phase;
    uint8_t events;
    uint8_t next_id;
    bool down_after_send;
    bool lcp_open;
    bool ipcp_open;
    // The ACCM the DUT asked for: the control characters the model must
    // escape once LCP is open.
    uint32_t peer_accm;
    struct ppp_fsm lcp;
    struct ppp_fsm ipcp;
    uint64_t terminate_due_us;
    uint8_t terminate_tries;
    uint8_t terminate_id;
    bool rx_in;
    bool rx_esc;
    bool rx_overflow;
    uint16_t rx_len;
    uint16_t tx_len;
    uint16_t tx_pos;
    uint8_t tx_stage;
    uint16_t ctl_len;
    uint8_t ctl[PPP_CTL_SIZE];
    uint8_t rx[PPP_MRU_MAX + PPP_FRAME_OVERHEAD];
    uint8_t tx[PPP_MRU_MAX + PPP_FRAME_OVERHEAD];
    struct ppp_stats stats;
};

// RFC 1662 C.2 FCS-16 of `len` bytes, starting from `fcs` (0xffff for a
// new frame). A frame with its FCS appended folds to PPP_FCS_GOOD.
uint16_t ppp_fcs(uint16_t fcs, const uint8_t *p, size_t len);
#define PPP_FCS_GOOD 0xf0b8u

void ppp_init(struct ppp *p, const struct ppp_settings *set, struct ip *ip);
// The link starts (CONNECT): passive, waiting for the DUT's LCP.
void ppp_open(struct ppp *p);
// The link stops at once, without a word on the wire.
void ppp_close(struct ppp *p);
// The model ends the link: LCP Terminate-Request, PPP_EV_DOWN on the
// DUT's Terminate-Ack or after the retries.
void ppp_terminate(struct ppp *p, uint64_t now_us);
bool ppp_running(const struct ppp *p);

void ppp_rx(struct ppp *p, const uint8_t *bytes, size_t len, uint64_t now_us);
// Writes up to `max` bytes of escaped frames into `out`; 0 when the link
// has nothing to send.
size_t ppp_pull(struct ppp *p, uint8_t *out, size_t max, uint64_t now_us);
// A frame is part written: the rest must follow on the same stream.
bool ppp_tx_busy(const struct ppp *p);
void ppp_tick(struct ppp *p, uint64_t now_us);

#endif
