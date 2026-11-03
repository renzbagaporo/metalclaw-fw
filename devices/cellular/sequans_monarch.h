// SPDX-License-Identifier: Apache-2.0 OR MIT
// Sequans Monarch LTE-M/NB-IoT (GM01Q platform, and the Monarch 2 GM02S):
// the AT command subset the model answers, from the Monarch Platform
// LR5.1.1.0 AT Commands Reference Manual Rev 6 ("LR5"), the Monarch 2
// Platform LR8.2 AT Commands Reference Manual Rev 3 ("LR8.2") and the
// GM02S Data Sheet Rev 12.

#ifndef MCLW_DEVICES_CELLULAR_SEQUANS_MONARCH_H
#define MCLW_DEVICES_CELLULAR_SEQUANS_MONARCH_H

#include <stdbool.h>
#include <stdint.h>

#include "metalclaw/model.h"

#include "modem.h"
#include "net.h"
#include "sms.h"

#define SEQUANS_MONARCH_DEFAULT_BAUD 115200

// Data sheet 3.1.2: RESETN is active low, and 100 us is the shortest pulse
// it reliably detects. Zephyr's sqn,gm02s sends its first command 2 s after
// releasing RESETN, without probing.
#define SEQUANS_MONARCH_RESET_US 100u
#define SEQUANS_MONARCH_DEFAULT_BOOT_MS 1500u

#define SEQUANS_MONARCH_MANUFACTURER "SEQUANS Communications"
#define SEQUANS_MONARCH_MODEL "GM02S"
#define SEQUANS_MONARCH_REVISION "UE8.2.0.5-60964"
// LR8.2 ATI: manufacturer, model and UE version on three lines.
#define SEQUANS_MONARCH_PRODUCT \
    SEQUANS_MONARCH_MANUFACTURER "\r\n" SEQUANS_MONARCH_MODEL "\r\n" SEQUANS_MONARCH_REVISION
#define SEQUANS_MONARCH_IMEI "354658090123453"
#define SEQUANS_MONARCH_IMSI "001010123456789"
#define SEQUANS_MONARCH_ICCID "89882280666027595036"
#define SEQUANS_MONARCH_OPERATOR "Metalclaw"

// Socket <connId> 1..6 (LR5 12.2.3), socket n on the network's socket n-1.
#define SEQUANS_MONARCH_SOCKETS 6
// PDP contexts 1..6, the <cid> range of +SQNSCFG; 0 there names none.
#define SEQUANS_MONARCH_CONTEXTS 6
#define SEQUANS_MONARCH_APN_MAX 32
// LR5 12.10 <maxByte> and 12.15 <bytesToSend>, and the <recData> of
// +SQNSRING.
#define SEQUANS_MONARCH_DATA_MAX 1500
// No manual gives an input timeout for +SQNSSENDEXT.
#define SEQUANS_MONARCH_SEND_INPUT_MS 10000u
// LR5 16.14: security profiles 1..6.
#define SEQUANS_MONARCH_PROFILES 6

#define SEQUANS_MONARCH_TEMPERATURE_MIN (-40)
#define SEQUANS_MONARCH_TEMPERATURE_MAX 85
#define SEQUANS_MONARCH_DEFAULT_TEMPERATURE 25

// The longest answer that is not clamped to the FIFO, an `AT+CGDCONT?`
// list of six contexts with 31-character APNs, and the longest URC,
// `+SQNSRING: 6,1500`.
#define SEQUANS_MONARCH_COMMAND_ROOM 400
#define SEQUANS_MONARCH_URC_ROOM 32

// The engine's two kinds hold one entry each, the socket ones one per
// socket, so the queue never fills.
#define SEQUANS_MONARCH_EVENTS (2 + 2 * SEQUANS_MONARCH_SOCKETS)

enum {
    SEQUANS_MONARCH_EV_SQNSRING = MODEM_EV_PER_ARG,
    SEQUANS_MONARCH_EV_SQNSH,
};

// LR8.2 <PDP_type>: the EPS ones.
enum sequans_monarch_pdp_type {
    SEQUANS_MONARCH_PDP_UNDEFINED,
    SEQUANS_MONARCH_PDP_IP,
    SEQUANS_MONARCH_PDP_IPV6,
    SEQUANS_MONARCH_PDP_IPV4V6,
};

struct sequans_monarch_context {
    uint8_t type;
    char apn[SEQUANS_MONARCH_APN_MAX];
};

// +SQNSCFG, +SQNSCFGEXT and +SQNSSCFG of one socket.
struct sequans_monarch_socket_cfg {
    uint16_t pkt_size;
    uint16_t max_to;
    uint16_t conn_to;
    uint8_t cid;
    uint8_t tx_to;
    uint8_t sr_mode;
    uint8_t recv_mode;
    uint8_t keepalive;
    uint8_t listen_auto;
    uint8_t send_mode;
    uint8_t tls;
    uint8_t profile;
};

// What the user sets: kept by reset(), restored by reset(factory).
struct sequans_monarch_settings {
    int16_t temperature;
};

// What the part keeps in NVRAM: kept across power cycles, cleared by
// reset().
struct sequans_monarch_nv {
    struct sequans_monarch_context contexts[SEQUANS_MONARCH_CONTEXTS];
    struct sequans_monarch_socket_cfg sockets[SEQUANS_MONARCH_SOCKETS];
    uint16_t ips_timeout;
    uint8_t ctzr;
    uint8_t ctzu;
    uint8_t ips_mode;
    // The <fun> a reset with AT+CFUN=<fun>,1 comes back at.
    uint8_t boot_cfun;
};

// +SQNSI counters of one socket since it was dialled.
struct sequans_monarch_conn {
    uint32_t sent;
    uint32_t read;
};

// What the part holds while powered beyond the engine's and the network's:
// cleared by every power transition.
struct sequans_monarch_session {
    struct sequans_monarch_conn conns[SEQUANS_MONARCH_SOCKETS];
    // Bytes the open +SQNSSENDEXT still takes, and its half-read hex digit.
    uint16_t send_left;
    uint8_t hex_high;
    bool hex_half;
    // Bit n: context n is active by CGACT.
    uint8_t active;
    // The context the PPP call holds up, 0 for none.
    uint8_t ppp_cid;
};

struct sequans_monarch {
    struct modem m;
    struct net net;
    struct sms sms;
    struct modem_link link;
    struct sequans_monarch_settings set;
    struct sequans_monarch_nv nv;
    struct sequans_monarch_session ses;
    struct modem_event events[SEQUANS_MONARCH_EVENTS];
    struct net_socket sockets[SEQUANS_MONARCH_SOCKETS];
};

struct sequans_monarch *sequans_monarch_of(struct modem *m);
// The context the bearer serves: defined, active and the bearer up.
bool sequans_monarch_context_up(const struct sequans_monarch *s, uint8_t cid);

// The SQNS* sockets (LR5 chapter 12).
extern const struct modem_commands sequans_monarch_socket_commands;
void sequans_monarch_socket_event(struct sequans_monarch *s, uint8_t kind, uint8_t arg);
// The bearer went away: every socket ends at once without its pending
// data, each with its +SQNSH.
void sequans_monarch_drop_sockets(struct sequans_monarch *s);
void sequans_monarch_net_notify(void *ctx, uint8_t sock, enum net_notice notice);
void sequans_monarch_socket_defaults(struct sequans_monarch_socket_cfg *cfg);

extern const struct mclw_model mclw_model_sequans_monarch;

#endif
