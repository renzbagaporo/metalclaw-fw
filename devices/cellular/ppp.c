// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ip.h"
#include "net.h"
#include "ppp.h"

#define HDLC_FLAG 0x7eu
#define HDLC_ESCAPE 0x7du
#define HDLC_XOR 0x20u
#define HDLC_ADDRESS 0xffu
#define HDLC_CONTROL 0x03u
#define ACCM_ALL 0xffffffffu

// RFC 1661 5 codes; Echo and Discard are LCP's only.
#define CODE_CONFIGURE_REQUEST 1u
#define CODE_CONFIGURE_ACK 2u
#define CODE_CONFIGURE_NAK 3u
#define CODE_CONFIGURE_REJECT 4u
#define CODE_TERMINATE_REQUEST 5u
#define CODE_TERMINATE_ACK 6u
#define CODE_CODE_REJECT 7u
#define CODE_PROTOCOL_REJECT 8u
#define CODE_ECHO_REQUEST 9u
#define CODE_ECHO_REPLY 10u
#define CODE_DISCARD_REQUEST 11u

#define PACKET_HEADER 4u

#define LCP_MRU 1u
#define LCP_ACCM 2u
#define LCP_MAGIC 5u
#define IPCP_ADDRESS 3u
// RFC 1877.
#define IPCP_DNS1 129u

enum {
    TX_OPEN,
    TX_BODY,
    TX_CLOSE,
};

uint16_t ppp_fcs(uint16_t fcs, const uint8_t *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        fcs ^= p[i];
        for (int b = 0; b < 8; b++) {
            fcs = (fcs & 1u) != 0 ? (uint16_t)((fcs >> 1) ^ 0x8408u) : (uint16_t)(fcs >> 1);
        }
    }
    return fcs;
}

void ppp_init(struct ppp *p, const struct ppp_settings *set, struct ip *ip) {
    memset(p, 0, sizeof(*p));
    p->set = set;
    p->ip = ip;
}

static void restart(struct ppp *p, uint8_t phase) {
    const struct ppp_settings *set = p->set;
    struct ip *ip = p->ip;
    struct ppp_stats keep = p->stats;

    memset(p, 0, sizeof(*p));
    p->set = set;
    p->ip = ip;
    p->stats = keep;
    p->phase = phase;
    p->peer_accm = ACCM_ALL;
    p->next_id = 1;
}

void ppp_open(struct ppp *p) {
    ip_close(p->ip);
    restart(p, PPP_ESTABLISH);
}

void ppp_close(struct ppp *p) {
    ip_close(p->ip);
    restart(p, PPP_DEAD);
}

bool ppp_running(const struct ppp *p) {
    return p->phase != PPP_DEAD;
}

bool ppp_tx_busy(const struct ppp *p) {
    return p->tx_len != 0;
}

// The link is no longer negotiated: nothing reopens it but a new LCP
// exchange.
static void forget(struct ppp *p) {
    p->lcp_open = false;
    p->ipcp_open = false;
    memset(&p->lcp, 0, sizeof(p->lcp));
    memset(&p->ipcp, 0, sizeof(p->ipcp));
    ip_close(p->ip);
}

static void down(struct ppp *p) {
    forget(p);
    p->phase = PPP_DEAD;
    p->lcp_open = false;
    p->ipcp_open = false;
    p->ctl_len = 0;
    p->down_after_send = false;
    p->events |= PPP_EV_DOWN;
}

static void ctl_push(struct ppp *p, uint16_t proto, uint8_t code, uint8_t id, const uint8_t *data, size_t len) {
    size_t total = 2u + PACKET_HEADER + len;

    if (p->ctl_len + total > PPP_CTL_SIZE) {
        p->stats.drops++;
        return;
    }
    uint8_t *q = &p->ctl[p->ctl_len];
    ip_put16(&q[0], proto);
    q[2] = code;
    q[3] = id;
    ip_put16(&q[4], (uint16_t)(PACKET_HEADER + len));
    if (len > 0) {
        memcpy(&q[6], data, len);
    }
    p->ctl_len = (uint16_t)(p->ctl_len + total);
}

static void request(struct ppp *p, struct ppp_fsm *fsm, uint16_t proto, uint64_t now_us) {
    uint8_t opts[10];
    size_t n = 0;

    if (proto == PPP_PROTO_LCP) {
        opts[n++] = LCP_MRU;
        opts[n++] = 4;
        ip_put16(&opts[n], p->set->mru);
        n += 2;
        opts[n++] = LCP_ACCM;
        opts[n++] = 6;
        ip_put32(&opts[n], 0);
        n += 4;
    } else {
        opts[n++] = IPCP_ADDRESS;
        opts[n++] = 6;
        memcpy(&opts[n], p->set->gw, 4);
        n += 4;
    }
    fsm->id = p->next_id++;
    fsm->requested = true;
    fsm->ack_rcvd = false;
    fsm->tries++;
    fsm->due_us = now_us + PPP_RESTART_US;
    ctl_push(p, proto, CODE_CONFIGURE_REQUEST, fsm->id, opts, n);
}

static void check_open(struct ppp *p) {
    if (!p->lcp_open && p->lcp.ack_sent && p->lcp.ack_rcvd) {
        p->lcp_open = true;
        p->phase = PPP_NETWORK;
    }
    if (p->lcp_open && !p->ipcp_open && p->ipcp.ack_sent && p->ipcp.ack_rcvd) {
        p->ipcp_open = true;
        ip_open(p->ip, p->set->mru);
        p->events |= PPP_EV_UP;
    }
}

// Walks options; false when one runs past the packet.
static bool options_valid(const uint8_t *opts, size_t len) {
    size_t i = 0;

    while (i < len) {
        if (i + 2 > len || opts[i + 1] < 2 || i + opts[i + 1] > len) {
            return false;
        }
        i += opts[i + 1];
    }
    return true;
}

// Zephyr checks that the Configure-Ack is an exact copy of its request
// (net/l2/ppp/fsm.c), so the model acknowledges with the options it got.
static void lcp_configure(struct ppp *p, uint8_t id, const uint8_t *opts, size_t len, uint64_t now_us) {
    uint8_t reject[PPP_CTL_SIZE];
    size_t rejected = 0;
    uint32_t accm = ACCM_ALL;

    if (!options_valid(opts, len) || len > sizeof(reject)) {
        return;
    }
    if (p->lcp_open) {
        p->lcp_open = false;
        p->ipcp_open = false;
        p->phase = PPP_ESTABLISH;
        memset(&p->lcp, 0, sizeof(p->lcp));
        memset(&p->ipcp, 0, sizeof(p->ipcp));
        ip_close(p->ip);
    }
    for (size_t i = 0; i < len; i += opts[i + 1]) {
        uint8_t type = opts[i];
        uint8_t olen = opts[i + 1];
        if (type == LCP_ACCM && olen == 6) {
            accm = ip_get32(&opts[i + 2]);
        } else if (!((type == LCP_MRU && olen == 4) || (type == LCP_MAGIC && olen == 6))) {
            memcpy(&reject[rejected], &opts[i], olen);
            rejected += olen;
        }
    }
    if (rejected > 0) {
        ctl_push(p, PPP_PROTO_LCP, CODE_CONFIGURE_REJECT, id, reject, rejected);
    } else {
        ctl_push(p, PPP_PROTO_LCP, CODE_CONFIGURE_ACK, id, opts, len);
        p->peer_accm = accm;
        p->lcp.ack_sent = true;
    }
    if (!p->lcp.requested) {
        p->lcp.tries = 0;
        request(p, &p->lcp, PPP_PROTO_LCP, now_us);
    }
}

// The dance of the spec's C.3: reject what is not the address or the
// first DNS server, then NAK each with the value the DUT is to take, then
// acknowledge the request that carries them.
static void ipcp_configure(struct ppp *p, uint8_t id, const uint8_t *opts, size_t len, uint64_t now_us) {
    uint8_t reject[PPP_CTL_SIZE];
    uint8_t nak[PPP_CTL_SIZE];
    size_t rejected = 0;
    size_t naked = 0;

    if (!options_valid(opts, len) || len > sizeof(reject)) {
        return;
    }
    for (size_t i = 0; i < len; i += opts[i + 1]) {
        uint8_t type = opts[i];
        uint8_t olen = opts[i + 1];
        const uint8_t *want = type == IPCP_ADDRESS ? p->ip->net->set.ip : (type == IPCP_DNS1 ? p->set->dns : NULL);
        if (want == NULL || olen != 6) {
            memcpy(&reject[rejected], &opts[i], olen);
            rejected += olen;
        } else if (memcmp(&opts[i + 2], want, 4) != 0) {
            nak[naked++] = type;
            nak[naked++] = 6;
            memcpy(&nak[naked], want, 4);
            naked += 4;
        }
    }
    if (rejected > 0) {
        ctl_push(p, PPP_PROTO_IPCP, CODE_CONFIGURE_REJECT, id, reject, rejected);
    } else if (naked > 0) {
        ctl_push(p, PPP_PROTO_IPCP, CODE_CONFIGURE_NAK, id, nak, naked);
    } else {
        ctl_push(p, PPP_PROTO_IPCP, CODE_CONFIGURE_ACK, id, opts, len);
        p->ipcp.ack_sent = true;
    }
    if (!p->ipcp.requested) {
        p->ipcp.tries = 0;
        request(p, &p->ipcp, PPP_PROTO_IPCP, now_us);
    }
}

static void terminated_by_peer(struct ppp *p, uint16_t proto, uint8_t id) {
    ctl_push(p, proto, CODE_TERMINATE_ACK, id, NULL, 0);
    if (proto == PPP_PROTO_IPCP) {
        p->ipcp_open = false;
        memset(&p->ipcp, 0, sizeof(p->ipcp));
        ip_close(p->ip);
        return;
    }
    p->stats.terminates++;
    forget(p);
    p->phase = PPP_TERMINATE;
    p->down_after_send = true;
}

static void control(struct ppp *p, uint16_t proto, const uint8_t *pkt, size_t len, uint64_t now_us) {
    bool lcp = proto == PPP_PROTO_LCP;
    struct ppp_fsm *fsm = lcp ? &p->lcp : &p->ipcp;

    if (len < PACKET_HEADER) {
        return;
    }
    uint8_t code = pkt[0];
    uint8_t id = pkt[1];
    size_t plen = ip_get16(&pkt[2]);
    if (plen < PACKET_HEADER || plen > len) {
        return;
    }
    const uint8_t *data = &pkt[PACKET_HEADER];
    size_t dlen = plen - PACKET_HEADER;
    if (p->phase == PPP_TERMINATE && code != CODE_TERMINATE_REQUEST && code != CODE_TERMINATE_ACK) {
        return;
    }
    switch (code) {
        case CODE_CONFIGURE_REQUEST:
            if (lcp) {
                lcp_configure(p, id, data, dlen, now_us);
            } else {
                ipcp_configure(p, id, data, dlen, now_us);
            }
            break;
        case CODE_CONFIGURE_ACK:
            if (fsm->requested && id == fsm->id) {
                fsm->ack_rcvd = true;
            }
            break;
        case CODE_CONFIGURE_NAK:
        case CODE_CONFIGURE_REJECT:
            if (fsm->requested && id == fsm->id && !fsm->ack_rcvd) {
                request(p, fsm, proto, now_us);
            }
            break;
        case CODE_TERMINATE_REQUEST:
            terminated_by_peer(p, proto, id);
            break;
        case CODE_TERMINATE_ACK:
            if (lcp && p->phase == PPP_TERMINATE && !p->down_after_send && id == p->terminate_id) {
                down(p);
            }
            break;
        case CODE_ECHO_REQUEST:
            // RFC 1661 5.8: the reply carries the model's magic number,
            // zero since it negotiated none.
            if (lcp && p->lcp_open && dlen >= 4 && dlen <= PPP_CTL_SIZE - 2u - PACKET_HEADER) {
                uint8_t reply[PPP_CTL_SIZE];
                memset(reply, 0, 4);
                memcpy(&reply[4], &data[4], dlen - 4);
                ctl_push(p, PPP_PROTO_LCP, CODE_ECHO_REPLY, id, reply, dlen);
            }
            break;
        case CODE_CODE_REJECT:
        case CODE_PROTOCOL_REJECT:
        case CODE_ECHO_REPLY:
        case CODE_DISCARD_REQUEST:
            break;
        default: {
            // RFC 1661 5.6: Code-Reject with as much of the packet as fits.
            size_t n = plen;
            if (n > PPP_CTL_SIZE / 2u) {
                n = PPP_CTL_SIZE / 2u;
            }
            ctl_push(p, proto, CODE_CODE_REJECT, p->next_id++, pkt, n);
            break;
        }
    }
    check_open(p);
}

static void protocol_reject(struct ppp *p, uint16_t proto, const uint8_t *info, size_t len) {
    uint8_t data[PPP_CTL_SIZE / 2u];
    size_t n = len < sizeof(data) - 2u ? len : sizeof(data) - 2u;

    ip_put16(data, proto);
    memcpy(&data[2], info, n);
    ctl_push(p, PPP_PROTO_LCP, CODE_PROTOCOL_REJECT, p->next_id++, data, n + 2u);
}

static void frame_end(struct ppp *p, uint64_t now_us) {
    size_t len = p->rx_len;

    if (p->rx_overflow) {
        p->stats.drops++;
        return;
    }
    // A run of flags, or an aborted frame.
    if (len < PPP_FRAME_OVERHEAD) {
        return;
    }
    if (ppp_fcs(0xffffu, p->rx, len) != PPP_FCS_GOOD) {
        p->stats.bad_fcs++;
        return;
    }
    if (p->rx[0] != HDLC_ADDRESS || p->rx[1] != HDLC_CONTROL || p->phase == PPP_DEAD) {
        return;
    }
    uint16_t proto = ip_get16(&p->rx[2]);
    const uint8_t *info = &p->rx[4];
    size_t n = len - PPP_FRAME_OVERHEAD;
    if (proto == PPP_PROTO_LCP || (proto == PPP_PROTO_IPCP && p->lcp_open)) {
        control(p, proto, info, n, now_us);
    } else if (proto == PPP_PROTO_IP) {
        if (p->ipcp_open) {
            ip_input(p->ip, info, n, now_us);
        }
    } else if (p->lcp_open) {
        protocol_reject(p, proto, info, n);
    }
}

void ppp_rx(struct ppp *p, const uint8_t *bytes, size_t len, uint64_t now_us) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = bytes[i];
        if (b == HDLC_FLAG) {
            if (p->rx_in && p->rx_len > 0) {
                frame_end(p, now_us);
            }
            p->rx_in = true;
            p->rx_len = 0;
            p->rx_esc = false;
            p->rx_overflow = false;
            continue;
        }
        // Bytes outside a frame (a stray line before the first flag).
        if (!p->rx_in) {
            continue;
        }
        if (b == HDLC_ESCAPE) {
            p->rx_esc = true;
            continue;
        }
        if (p->rx_esc) {
            b ^= HDLC_XOR;
            p->rx_esc = false;
        }
        if (p->rx_len < sizeof(p->rx)) {
            p->rx[p->rx_len++] = b;
        } else {
            p->rx_overflow = true;
        }
    }
}

void ppp_terminate(struct ppp *p, uint64_t now_us) {
    if (p->phase == PPP_DEAD || p->phase == PPP_TERMINATE) {
        return;
    }
    p->stats.terminates++;
    forget(p);
    p->phase = PPP_TERMINATE;
    p->ctl_len = 0;
    p->terminate_id = p->next_id++;
    p->terminate_tries = 1;
    p->terminate_due_us = now_us + PPP_RESTART_US;
    ctl_push(p, PPP_PROTO_LCP, CODE_TERMINATE_REQUEST, p->terminate_id, NULL, 0);
}

static void fsm_tick(struct ppp *p, struct ppp_fsm *fsm, uint16_t proto, uint64_t now_us) {
    if (!fsm->requested || fsm->ack_rcvd || now_us < fsm->due_us) {
        return;
    }
    if (fsm->tries >= PPP_CONFIGURE_TRIES) {
        down(p);
        return;
    }
    request(p, fsm, proto, now_us);
}

void ppp_tick(struct ppp *p, uint64_t now_us) {
    switch (p->phase) {
        case PPP_ESTABLISH:
            fsm_tick(p, &p->lcp, PPP_PROTO_LCP, now_us);
            break;
        case PPP_NETWORK:
            fsm_tick(p, &p->lcp, PPP_PROTO_LCP, now_us);
            if (p->phase == PPP_NETWORK) {
                fsm_tick(p, &p->ipcp, PPP_PROTO_IPCP, now_us);
            }
            if (p->ipcp_open) {
                ip_tick(p->ip, now_us);
            }
            break;
        case PPP_TERMINATE:
            if (!p->down_after_send && now_us >= p->terminate_due_us) {
                if (p->terminate_tries >= PPP_TERMINATE_TRIES) {
                    down(p);
                } else {
                    p->terminate_tries++;
                    p->terminate_due_us = now_us + PPP_RESTART_US;
                    ctl_push(p, PPP_PROTO_LCP, CODE_TERMINATE_REQUEST, p->terminate_id, NULL, 0);
                }
            }
            break;
        default:
            break;
    }
}

// The next frame into `tx`: a control packet first, then an IP packet.
static bool next_frame(struct ppp *p, uint64_t now_us) {
    size_t info;
    uint16_t proto;

    if (p->ctl_len > 0) {
        proto = ip_get16(p->ctl);
        info = ip_get16(&p->ctl[4]);
        memcpy(&p->tx[4], &p->ctl[2], info);
        size_t used = 2u + info;
        memmove(p->ctl, &p->ctl[used], p->ctl_len - used);
        p->ctl_len = (uint16_t)(p->ctl_len - used);
    } else if (p->phase == PPP_NETWORK && p->ipcp_open) {
        info = ip_output(p->ip, &p->tx[4], p->ip->mtu, now_us);
        proto = PPP_PROTO_IP;
        if (info == 0) {
            return false;
        }
    } else {
        return false;
    }
    p->tx[0] = HDLC_ADDRESS;
    p->tx[1] = HDLC_CONTROL;
    ip_put16(&p->tx[2], proto);
    uint16_t fcs = (uint16_t)~ppp_fcs(0xffffu, p->tx, 4u + info);
    p->tx[4u + info] = (uint8_t)fcs;
    p->tx[5u + info] = (uint8_t)(fcs >> 8);
    p->tx_len = (uint16_t)(PPP_FRAME_OVERHEAD + info);
    p->tx_pos = 0;
    p->tx_stage = TX_OPEN;
    return true;
}

// RFC 1662 7.1: every control character until LCP is open, then those
// the DUT's ACCM names.
static bool needs_escape(const struct ppp *p, uint8_t b) {
    uint32_t accm = p->lcp_open ? p->peer_accm : ACCM_ALL;

    return b == HDLC_FLAG || b == HDLC_ESCAPE || (b < 0x20u && ((accm >> b) & 1u) != 0);
}

size_t ppp_pull(struct ppp *p, uint8_t *out, size_t max, uint64_t now_us) {
    size_t n = 0;

    while (n < max) {
        if (p->tx_len == 0) {
            if (p->down_after_send && p->ctl_len == 0) {
                down(p);
            }
            if (!next_frame(p, now_us)) {
                break;
            }
        }
        if (p->tx_stage == TX_OPEN) {
            out[n++] = HDLC_FLAG;
            p->tx_stage = TX_BODY;
            continue;
        }
        if (p->tx_stage == TX_BODY) {
            while (p->tx_pos < p->tx_len) {
                uint8_t b = p->tx[p->tx_pos];
                // Zephyr escapes the control field whatever the ACCM
                // (modem_ppp.c), so the model's frames read the same.
                bool esc = needs_escape(p, b) || p->tx_pos == 1;
                if (n + (esc ? 2u : 1u) > max) {
                    return n;
                }
                if (esc) {
                    out[n++] = HDLC_ESCAPE;
                    b ^= HDLC_XOR;
                }
                out[n++] = b;
                p->tx_pos++;
            }
            p->tx_stage = TX_CLOSE;
            continue;
        }
        out[n++] = HDLC_FLAG;
        p->tx_len = 0;
    }
    if (p->tx_len == 0 && p->down_after_send && p->ctl_len == 0) {
        down(p);
    }
    return n;
}
