// SPDX-License-Identifier: Apache-2.0 OR MIT
// SIMCom SIM7000 series (LTE-M/NB-IoT/GPRS): the AT command subset the model
// answers, from the SIM7000 Series AT Command Manual V1.06, the TCP/IP and
// GNSS application notes and the SIM7000 Hardware Design V1.07, as TinyGSM's
// SIM7000 (CIP stack) and SIM7000SSL (CNACT/CA stack) drivers use them.

#ifndef MCLW_DEVICES_CELLULAR_SIM7000_H
#define MCLW_DEVICES_CELLULAR_SIM7000_H

#include <stdbool.h>
#include <stdint.h>

#include "metalclaw/model.h"

#include "gnss.h"
#include "modem.h"
#include "net.h"
#include "sms.h"

#define SIM7000_DEFAULT_BAUD 115200

// Hardware design 3.2.1 table 8 and 3.2.2 table 9: PWRKEY low for at least
// 1 s powers on (Ton), at least 1.2 s powers off (Toff); the UART is ready
// at least 4.5 s after the power-on issue.
#define SIM7000_PWRKEY_ON_US 1000000u
#define SIM7000_PWRKEY_OFF_US 1200000u
#define SIM7000_DEFAULT_BOOT_MS 4500u

#define SIM7000_MANUFACTURER "SIMCOM_Ltd"
#define SIM7000_MODEL "SIMCOM_SIM7000G"
#define SIM7000_REVISION "Revision:1351B03SIM7000G"
// Manual 2.2.5 example.
#define SIM7000_PRODUCT "SIM7000 R1351"
#define SIM7000_IMEI "862818040001234"
#define SIM7000_IMSI "001010123456789"
#define SIM7000_ICCID "89882280666027595036"
#define SIM7000_OPERATOR "Metalclaw"

// TinyGSM's SIM7000 has eight CIP connections (SIM7000.h:15), the SSL driver
// two CA ones (SIM7000SSL.h:15). They share the slots of the network; a slot
// belongs to the stack that opened it.
#define SIM7000_SOCKETS 8
#define SIM7000_CA_CIDS 2
// Manual 8.2.27: <reqlength> 1..1460; 8.2.3 and 18.2.5 the same for sends.
#define SIM7000_DATA_MAX 1460
// Manual 18.2.5: the default <inputtime>; the CIPSEND data phase waits as long.
#define SIM7000_INPUT_MS 5000u
#define SIM7000_APN_MAX 32
// Manual 7.2.1: SAPBR profiles 1..3; only 1 is modelled.
#define SIM7000_SAPBR_CID 1

// The answers that trail their command, when the manual shows them after the
// OK: +CNTP and the connection results.
#define SIM7000_URC_DELAY_US 50000u

// The longest answer that is not clamped to the FIFO: AT+CIPSTATUS with the
// C: line of all eight connections. The longest URC: +SAPBR 1: DEACT.
#define SIM7000_COMMAND_ROOM 512
#define SIM7000_URC_ROOM 48

// The engine's two kinds, five global kinds and three kinds per connection.
#define SIM7000_EVENTS (2 + 5 + 3 * SIM7000_SOCKETS)

enum {
    SIM7000_EV_APP_PDP = MODEM_EV_MODEL,
    SIM7000_EV_NTP,
    SIM7000_EV_CUSD,
    SIM7000_EV_PDP_DEACT,
    SIM7000_EV_SAPBR_DEACT,
    // +CIPRXGET: 1,<n> or +CADATAIND: <cid>.
    SIM7000_EV_DATA = MODEM_EV_PER_ARG,
    // <n>, CLOSED or +CASTATE: <cid>,0.
    SIM7000_EV_CLOSED,
    // <n>, CONNECT OK or CONNECT FAIL.
    SIM7000_EV_CONNECT,
};

// Manual 8.2.2 <state>, as AT+CIPSTATUS reports it in multi-IP mode.
enum sim7000_cip_state {
    SIM7000_CIP_INITIAL,
    SIM7000_CIP_START,
    SIM7000_CIP_GPRSACT,
    SIM7000_CIP_STATUS,
    SIM7000_CIP_PDP_DEACT,
};

// Who holds the data bearer (the module's PDP context): bits of
// `sim7000_session.bearers`, which keep `net.ses.bearer` up while any is set.
#define SIM7000_BEARER_CIP 0x01u
#define SIM7000_BEARER_SAPBR 0x02u
#define SIM7000_BEARER_CNACT 0x04u

enum sim7000_owner {
    SIM7000_OWNER_NONE,
    SIM7000_OWNER_CIP,
    SIM7000_OWNER_CA,
};

// The result of a CIPSTART, reported as `<n>, CONNECT OK`, `CONNECT FAIL` or
// `ALREADY CONNECT` (manual 8.2.2).
#define SIM7000_CONNECT_OK 0
#define SIM7000_CONNECT_FAIL 1
#define SIM7000_CONNECT_ALREADY 2

// Manual 18.2.3 <result> of +CAOPEN.
#define SIM7000_CAOPEN_OK 0
#define SIM7000_CAOPEN_SOCKET_ERROR 1
#define SIM7000_CAOPEN_CONNECTION_LIMIT 3
#define SIM7000_CAOPEN_PARAMETER_INVALID 4
#define SIM7000_CAOPEN_DNS_FAILED 20
#define SIM7000_CAOPEN_NETWORK_NOT_ACTIVE 21
#define SIM7000_CAOPEN_REMOTE_REFUSED 23

// Manual 11.2.2 <code> of +CNTP.
#define SIM7000_CNTP_OK 1
#define SIM7000_CNTP_NETWORK_ERROR 61

// <locationcode> of +CLBS, which the SIM7000 manual does not have; the fake
// cell's accuracy.
#define SIM7000_CLBS_OK 0
#define SIM7000_CLBS_NET_ERROR 3
#define SIM7000_CLBS_ACCURACY_M 500

// What the part holds while powered beyond the engine's and the network's:
// cleared by every power transition.
struct sim7000_session {
    uint8_t owner[SIM7000_SOCKETS];
    uint8_t cip_state;
    uint8_t bearers;
    // What the CIPSTART on slot n reports after its OK: a SIM7000_CONNECT_.
    uint8_t connect[SIM7000_SOCKETS];
    uint8_t cip_mux;
    uint8_t cip_qsend;
    uint8_t rxget;
    uint8_t ca_cid;
    uint8_t sslversion;
    uint16_t sent;
    int8_t ntp_tz;
    bool ntp_tz_set;
    bool cgact;
    char apn[SIM7000_APN_MAX];
};

struct sim7000 {
    struct modem m;
    struct net net;
    struct sms sms;
    struct gnss gnss;
    struct sim7000_session ses;
    struct modem_event events[SIM7000_EVENTS];
    struct net_socket sockets[SIM7000_SOCKETS];
};

struct sim7000 *sim7000_of(struct modem *m);

// The CIP stack (manual chapter 8) and the CA stack (chapter 18).
extern const struct modem_commands sim7000_cip_commands;
extern const struct modem_commands sim7000_ca_commands;

// The numeric argument `i`, from a number or a quoted one.
bool sim7000_arg_int(const struct modem_args *a, uint8_t i, int32_t lo, int32_t hi, int32_t *out);
void sim7000_set_apn(struct sim7000 *s, const char *apn);
// Takes up to `n` bytes the server sent into the output FIFO.
void sim7000_emit(struct modem *m, uint8_t slot, size_t n);
// Gives the bearer to, or takes it from, one of the SIM7000_BEARER_ holders.
void sim7000_bearer(struct sim7000 *s, uint8_t holder, bool up);
// Ends the slot's connection at once and forgets its URCs.
void sim7000_forget_socket(struct sim7000 *s, uint8_t slot);
// The network ended every connection of `owner` (NONE: of both) at once,
// without their pending data; the ones that were open report it.
void sim7000_drop_sockets(struct sim7000 *s, uint8_t owner);
void sim7000_socket_event(struct sim7000 *s, uint8_t kind, uint8_t arg);
void sim7000_net_notify(void *ctx, uint8_t sock, enum net_notice notice);
// AT+CIPSHUT: the CIP stack back to IP INITIAL.
void sim7000_cip_shut(struct sim7000 *s);

extern const struct mclw_model mclw_model_sim7000;

#endif
