// SPDX-License-Identifier: Apache-2.0 OR MIT
// SIMCom A7672X (A76XX series, LTE Cat-1): the AT command subset the model
// answers, from the A76XX Series AT Command Manual V2.04, the A76XX Series
// TCPIP AT Command Manual V1.00 and the A7672X Series Hardware Design V1.10.

#ifndef MCLW_DEVICES_CELLULAR_A7672X_H
#define MCLW_DEVICES_CELLULAR_A7672X_H

#include <stdbool.h>
#include <stdint.h>

#include "metalclaw/model.h"

#include "modem.h"
#include "net.h"
#include "sms.h"

#define A7672X_DEFAULT_BAUD 115200

// Hardware design 3.2.1 and 3.2.2: PWRKEY low for at least 50 ms powers on
// (Ton), at least 2.5 s powers off (Toff); the UART is ready at most 10 s
// after power on (Ton(uart)).
#define A7672X_PWRKEY_ON_US 50000u
#define A7672X_PWRKEY_OFF_US 2500000u
#define A7672X_DEFAULT_BOOT_MS 5000u

// The time the reply of a network service (+CNTP, +CLBS, +CUSD) trails the
// OK of its command; the socket and SSL services answer right after the OK.
// Socket data and server closes arrive a network round trip after the
// bytes that caused them, so a command's own result (+CIPSEND, +CCHSEND)
// always comes first.
#define A7672X_REPLY_DELAY_US 100000u
#define A7672X_NET_DELAY_US 10000u

#define A7672X_MANUFACTURER "SIMCOM INCORPORATED"
#define A7672X_MODEL "A7672E-FASE"
#define A7672X_FIRMWARE "A011B07A7672M7"
#define A7672X_IMEI "862205051234567"
#define A7672X_IMSI "001010123456789"
#define A7672X_ICCID "89882280666027595044"
#define A7672X_OPERATOR "Metalclaw"

// Manual 15.2.3: links 0..9. The model backs the first four with sockets.
#define A7672X_LINK_MAX 9
#define A7672X_LINKS 4
// Manual 19.2.12: the session count "varies by product model"; TinyGSM's
// A7672X client and the manual's examples use 0 and 1.
#define A7672X_SESSIONS 2
#define A7672X_SOCKETS (A7672X_LINKS + A7672X_SESSIONS)
// Manual 19.2.1: SSL contexts 0..9.
#define A7672X_SSL_CTX_MAX 9
#define A7672X_CIPSEND_MAX 1500
#define A7672X_CCHSEND_MAX 2048
#define A7672X_RXGET_ASCII_MAX 1500
#define A7672X_RXGET_HEX_MAX 750
#define A7672X_APN_MAX 32
#define A7672X_TEMPERATURE_MIN (-40)
#define A7672X_TEMPERATURE_MAX 125
#define A7672X_DEFAULT_TEMPERATURE 29
// Manual 14.2.5: the +CNTP <timezone> default and range, quarter hours.
#define A7672X_NTP_DEFAULT_TZ 32
#define A7672X_NTP_TZ_MAX 96
#define A7672X_CLBS_ACCURACY_M 550

// The longest answer that is not clamped to the FIFO, an `AT+CIPOPEN?`
// list with every socket open, and the longest URC, +CLBS type 4 at the
// extremes of both coordinates.
#define A7672X_COMMAND_ROOM 320
#define A7672X_URC_ROOM 80

// Manual 15.3.2 <err> of +NETOPEN, +NETCLOSE, +CIPOPEN, +CIPCLOSE and
// +CIPERROR.
#define A7672X_IP_OK 0
#define A7672X_IP_NETWORK_FAILURE 1
#define A7672X_IP_NOT_OPENED 2
#define A7672X_IP_WRONG_PARAMETER 3
#define A7672X_IP_NOT_SUPPORTED 4
#define A7672X_IP_CREATE_SOCKET 5
#define A7672X_IP_DNS_FAILED 11

// Manual 19.3.1 <err> of the CCH commands.
#define A7672X_CCH_OK 0
#define A7672X_CCH_DNS 13
#define A7672X_CCH_CONNECT 14
#define A7672X_CCH_NONET 17

// Manual 14.3.2 <err> of +CNTP: 0 is success, unlike the SIMCom 1.
#define A7672X_NTP_OK 0
#define A7672X_NTP_NETWORK 4

// Manual 38.2.1 <ret_code> of +CLBS.
#define A7672X_CLBS_OK 0
#define A7672X_CLBS_NONET 20

// Manual 4.2.3 <m> of +CUSD.
#define A7672X_CUSD_NOT_SUPPORTED 4

// +CIPEVENT, +CCH: CCH STOP and the replies after OK hold one entry each,
// a link's and a session's five kinds one per link or session.
enum {
    A7672X_EV_NETOPEN = MODEM_EV_MODEL,
    A7672X_EV_NETCLOSE,
    A7672X_EV_CIPEVENT,
    A7672X_EV_CCHSTART,
    A7672X_EV_CCHSTOP,
    A7672X_EV_CCH_STOPPED,
    A7672X_EV_CNTP,
    A7672X_EV_CUSD,
    A7672X_EV_CLBS,
    A7672X_EV_CIPOPEN = MODEM_EV_PER_ARG,
    A7672X_EV_CIPSEND,
    A7672X_EV_RXGET,
    A7672X_EV_IPCLOSE,
    A7672X_EV_CIPCLOSE,
    A7672X_EV_CCHOPEN,
    A7672X_EV_CCHSEND,
    A7672X_EV_CCHEVENT,
    A7672X_EV_CCH_PEER_CLOSED,
    A7672X_EV_CCHCLOSE,
};

#define A7672X_EVENTS (2 + 9 + 5 * A7672X_LINKS + 5 * A7672X_SESSIONS)

// The +NETOPEN/+NETCLOSE event argument: the <err>, and whether the
// command named its <cid> (the reply then carries it).
#define A7672X_WITH_CID 0x80
// The +CLBS event argument: the <type> asked for, or this with the error.
#define A7672X_CLBS_FAILED 0x80

// What the user sets: kept by reset(), restored by reset(factory).
struct a7672x_settings {
    int32_t latitude;
    int32_t longitude;
    int16_t temperature;
    bool ready_urc;
};

struct a7672x_link {
    uint32_t sent;
    uint32_t received;
    uint16_t send_len;
    uint8_t open_err;
    // The connection was gone when the CIPSEND data ended.
    bool send_lost;
};

struct a7672x_cch {
    uint8_t open_err;
    uint8_t send_err;
    uint8_t client_type;
    // The CCHSSLCFG context, or 0xff for the session's own index.
    uint8_t ssl_ctx;
};

// What the part holds while powered beyond the engine's and the network's:
// cleared by every power transition.
struct a7672x_session {
    struct a7672x_link links[A7672X_LINKS];
    struct a7672x_cch cch[A7672X_SESSIONS];
    uint8_t dns[2][4];
    uint8_t tcpka[4];
    uint8_t sslversion[A7672X_SSL_CTX_MAX + 1];
    uint16_t sni;
    uint8_t rxget_mode;
    uint8_t cch_report;
    uint8_t cch_recv_mode;
    uint8_t csclk;
    uint8_t cusd_n;
    int8_t ntp_tz;
    bool netopen;
    bool cch_started;
    // What holds context 1 up: CGACT, NETOPEN or CCHSTART, and the PPP
    // call. `net.ses.bearer` is up while either does.
    bool pdp_at;
    bool pdp_ppp;
    char apn[A7672X_APN_MAX];
};

struct a7672x {
    struct modem m;
    struct net net;
    struct modem_link link;
    struct sms sms;
    struct a7672x_settings set;
    struct a7672x_session ses;
    struct modem_event events[A7672X_EVENTS];
    struct net_socket sockets[A7672X_SOCKETS];
};

struct a7672x *a7672x_of(struct modem *m);

// The socket is the AT link or session's to use: not one the PPP link's
// TCP holds.
bool a7672x_sock_free(const struct a7672x *s, uint8_t sock);

// Context 1 is up: CGACT=1,1, NETOPEN or CCHSTART brought it up.
bool a7672x_activate(struct a7672x *s);
// The bearer went away: every link and session ends at once without its
// pending data and reports it, and the socket and SSL services hear of it.
void a7672x_drop_sockets(struct a7672x *s);

// NETOPEN, the CIP* links, CTCPKA, CDNSCFG and CDNSGIP (manual chapter 15).
extern const struct modem_commands a7672x_socket_commands;
void a7672x_socket_event(struct a7672x *s, uint8_t kind, uint8_t arg);
void a7672x_drop_links(struct a7672x *s);

// CSSLCFG and the CCH* sessions (manual chapter 19).
extern const struct modem_commands a7672x_ssl_commands;
void a7672x_ssl_event(struct a7672x *s, uint8_t kind, uint8_t arg);
void a7672x_drop_sessions(struct a7672x *s);

void a7672x_net_notify(void *ctx, uint8_t sock, enum net_notice notice);

extern const struct mclw_model mclw_model_a7672x;

#endif
