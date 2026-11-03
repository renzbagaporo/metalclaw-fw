// SPDX-License-Identifier: Apache-2.0 OR MIT
// SIMCom SIM7080G: the AT command subset the model answers, from the
// SIM7070/SIM7080/SIM7090 Series AT Command Manual V1.07 and the SIM7080G
// Hardware Design V1.06.

#ifndef MCLW_DEVICES_CELLULAR_SIM7080G_H
#define MCLW_DEVICES_CELLULAR_SIM7080G_H

#include <stdbool.h>
#include <stdint.h>

#include "metalclaw/model.h"

#include "gnss.h"
#include "modem.h"
#include "net.h"
#include "sms.h"

#define SIM7080G_DEFAULT_BAUD 115200

// Hardware design 4.2.1: PWRKEY low for at least 0.5 s powers on (Ton), at
// least 1.2 s powers off (Toff); the UART is ready about 3 s after power on.
#define SIM7080G_PWRKEY_ON_US 500000u
#define SIM7080G_PWRKEY_OFF_US 1200000u
#define SIM7080G_DEFAULT_BOOT_MS 3000u

// Zephyr's offload driver resets its PDP semaphore after the CNACT OK and
// its response semaphore after writing CASEND data, so both answers must
// trail the command (zephyr drivers/modem/simcom/sim7080, sim7080_pdp.c and
// sim7080_sock.c). The URC delay runs from the command, so it carries a
// margin for the OK's own time on the wire: the URC starts at least 100 ms
// after the OK's last byte.
#define SIM7080G_URC_DELAY_US 110000u
#define SIM7080G_CASEND_OK_DELAY_US 5000u
// Manual 12.2.5: the default <inputtime> of a CASEND data phase.
#define SIM7080G_CASEND_INPUT_MS 5000u

#define SIM7080G_MANUFACTURER "SIMCOM INCORPORATED"
#define SIM7080G_MODEL "SIMCOM_SIM7080"
#define SIM7080G_REVISION "Revision:1951B16SIM7080"
#define SIM7080G_PRODUCT "R1951.01"
#define SIM7080G_IMEI "869951030006302"
#define SIM7080G_IMSI "001010123456789"
#define SIM7080G_ICCID "89882280666027595036"
#define SIM7080G_OPERATOR "Metalclaw"
#define SIM7080G_MCC_MNC "001-01"

// Manual 7.2.1: four PDP contexts; the model activates only context 0.
#define SIM7080G_PDP_CONTEXTS 4
// Manual 12.2.1 allows cid 0..12; the Zephyr driver uses 0..4.
#define SIM7080G_CID_MAX 12
#define SIM7080G_SOCKETS 5
// Manual 12.2.5: <datalen> 1..1460.
#define SIM7080G_CASEND_MAX 1460
#define SIM7080G_APN_MAX 32

// The longest answer that is not clamped to the FIFO, an `AT+CAOPEN?`
// list, and the longest URC, +CDNSGIP with a 64-character host.
#define SIM7080G_COMMAND_ROOM 256
#define SIM7080G_URC_ROOM 112

// The engine's two kinds and the model's four global ones hold one entry
// each, the socket ones one per connection, so the queue never fills.
#define SIM7080G_EVENTS (6 + 2 * SIM7080G_SOCKETS)

// Manual 12.2.3 <result> of +CAOPEN.
#define SIM7080G_CAOPEN_OK 0
#define SIM7080G_CAOPEN_CONNECTION_LIMIT 3
#define SIM7080G_CAOPEN_PARAMETER_INVALID 4
#define SIM7080G_CAOPEN_NOT_SUPPORTED 7
#define SIM7080G_CAOPEN_NOT_RELEASED 9
#define SIM7080G_CAOPEN_ILLEGAL_OPERATION 10
#define SIM7080G_CAOPEN_DNS_FAILED 20
#define SIM7080G_CAOPEN_NETWORK_NOT_ACTIVE 21
#define SIM7080G_CAOPEN_REMOTE_REFUSED 23

// Manual 19.2.3 <err> of +CDNSGIP.
#define SIM7080G_DNS_INVALID_PARA 2
#define SIM7080G_DNS_NETWORK_ERROR 3
#define SIM7080G_DNS_ERROR_UNKNOWN 8

// Manual 16.2.2 <code> of +CNTP.
#define SIM7080G_CNTP_OK 1
#define SIM7080G_CNTP_NETWORK_ERROR 61

// Manual 15.2.14 <error> of +FTPGET: 1.
#define SIM7080G_FTP_DATA 1
#define SIM7080G_FTP_DONE 0
#define SIM7080G_FTP_NET_ERROR 61

// Manual 20.2.1 <locationcode> of +CLBS.
#define SIM7080G_CLBS_OK 0
#define SIM7080G_CLBS_NET_ERROR 3
#define SIM7080G_CLBS_ACCURACY_M 500

// Manual 5.2.16 and 5.2.17.
#define SIM7080G_CNMP_LTE 38
#define SIM7080G_CMNB_CATM 1
#define SIM7080G_CMNB_NBIOT 2
#define SIM7080G_CMNB_BOTH 3

enum {
    SIM7080G_EV_APP_PDP = MODEM_EV_MODEL,
    SIM7080G_EV_DNS,
    SIM7080G_EV_NTP,
    SIM7080G_EV_FTPGET,
    SIM7080G_EV_DATAIND = MODEM_EV_PER_ARG,
    SIM7080G_EV_CASTATE,
};

enum sim7080g_ftp_state {
    SIM7080G_FTP_IDLE,
    SIM7080G_FTP_STARTING,
    SIM7080G_FTP_ACTIVE,
};

// What the part holds while powered beyond the engine's and the network's:
// cleared by every power transition.
struct sim7080g_session {
    uint16_t ftp_off;
    uint16_t ftp_len;
    uint8_t cnmp;
    uint8_t cmnb;
    uint8_t ftp_state;
    uint8_t sslversion;
    uint8_t dns_err;
    uint8_t dns_ip[4];
    int8_t ntp_tz;
    bool ntp_tz_set;
    bool dns_ok;
    bool pdp_pending;
    char apn[SIM7080G_APN_MAX];
    char dns_host[NET_HOST_MAX + 1];
};

struct sim7080g {
    struct modem m;
    struct net net;
    struct sms sms;
    struct gnss gnss;
    struct sim7080g_session ses;
    struct modem_event events[SIM7080G_EVENTS];
    struct net_socket sockets[SIM7080G_SOCKETS];
};

struct sim7080g *sim7080g_of(struct modem *m);

// The CA* sockets and CDNSGIP (manual chapters 12 and 19).
extern const struct modem_commands sim7080g_socket_commands;
void sim7080g_socket_event(struct sim7080g *s, uint8_t kind, uint8_t arg);
// The bearer went away: every connection ends at once without its pending
// data, and its +CASTATE follows `urc_delay_us` from now.
void sim7080g_drop_sockets(struct sim7080g *s, uint32_t urc_delay_us);
void sim7080g_net_notify(void *ctx, uint8_t sock, enum net_notice notice);

extern const struct mclw_model mclw_model_sim7080g;

#endif
