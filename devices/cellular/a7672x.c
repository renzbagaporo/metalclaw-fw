// SPDX-License-Identifier: Apache-2.0 OR MIT
// The A7672X model: its descriptors and identity, power and boot, the PDP
// bearer (CGDCONT, CGACT, CGPADDR), radio queries, base-station location,
// NTP, USSD, the supply voltage and the temperature.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "a7672x.h"
#include "modem.h"
#include "net.h"
#include "sms.h"
#include "text.h"

enum {
    PROP_LATITUDE,
    PROP_LONGITUDE,
    PROP_TEMPERATURE,
    PROP_READY_URC,
    PROP_COUNT,
};

struct a7672x *a7672x_of(struct modem *m) {
    return (struct a7672x *)m;
}

bool a7672x_sock_free(const struct a7672x *s, uint8_t sock) {
    return !modem_link_owns_socket(&s->m, sock);
}

bool a7672x_activate(struct a7672x *s) {
    if (!modem_attached(&s->m)) {
        return false;
    }
    s->ses.pdp_at = true;
    s->net.ses.bearer = true;
    return true;
}

// The PPP call is on context 1, the only one modelled: CGACT? shows it up
// while the call is, and the call ending leaves it to whatever AT command
// raised it as well.
static void link_bearer(struct modem *m, bool up) {
    struct a7672x *s = a7672x_of(m);

    s->ses.pdp_ppp = up;
    s->net.ses.bearer = s->ses.pdp_at || s->ses.pdp_ppp;
}

void a7672x_drop_sockets(struct a7672x *s) {
    a7672x_drop_links(s);
    a7672x_drop_sessions(s);
}

// A command reset comes back at once: TinyGSM's restart sends CFUN=0,0
// straight after CRESET's OK and CFUN=1,1 after that, each with 10 s for
// its OK, and then allows 5 s for the module to answer AT; a real module's
// UART is gone for up to 10 s (hardware design Ton(uart)).
static void reboot_now(struct modem *m) {
    modem_power_on(m);
    m->boot_due_us = modem_now();
}

static modem_handler_t standard_handler(const char *name) {
    const struct modem_commands *t = &modem_standard_commands;

    for (size_t i = 0; i < t->count; i++) {
        if (strcmp(t->list[i].name, name) == 0) {
            return t->list[i].fn;
        }
    }
    return NULL;
}

// Manual 3.2.1: <rst> 1 takes effect only with <fun> 1, and CFUN=6 resets.
static void cmd_cfun(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t fun;
    int32_t rst = 0;

    if (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, 7, &fun) &&
        (a->count < 2 || modem_arg_int(a, 1, 0, 1, &rst))) {
        if (fun == 6 || (fun == 1 && rst == 1)) {
            modem_ok(m);
            reboot_now(m);
            return;
        }
        a->count = 1;
    }
    standard_handler("+CFUN")(m, mode, a);
}

static void cmd_creset(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode == MODEM_EXEC) {
        modem_ok(m);
        reboot_now(m);
    } else if (mode == MODEM_TEST) {
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

// Manual 3.2.5: OK, then POWERD DOWN; the +CGEV lines a real module sends
// in between are left out, as Zephyr's modem_cellular reads +CGEV as a
// registration change.
static void cmd_cpof(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t how;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CPOF: (0,1)");
    } else if (mode == MODEM_EXEC || (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, 1, &how))) {
        modem_ok(m);
        modem_power_off(m, true);
    } else {
        modem_error(m);
    }
}

// A command of the series this part answers ERROR to: CNTPCID does not
// exist on the A76XX, and the modelled variant has no GNSS (CGNSSPWR).
static void cmd_absent(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)mode;
    (void)a;
    modem_error(m);
}

static void cmd_ciccid(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (mode != MODEM_EXEC) {
        modem_error(m);
    } else if (m->set.sim == 1) {
        modem_cme(m, MODEM_CME_SIM_NOT_INSERTED);
    } else {
        modem_answer(m, "+ICCID: " A7672X_ICCID);
    }
}

static void cmd_cspn(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (mode != MODEM_READ) {
        modem_error(m);
    } else if (modem_require_sim(m)) {
        modem_answer(m, "+CSPN: \"" A7672X_OPERATOR "\",0");
    }
}

// 27.007 8.69, which Zephyr's cellular_get_signal() sends for RSRP and
// RSRQ. RSRP is 10 dB below the rssi property's -113 + 2n dBm, RSRQ a
// fixed -10 dB, each coded as 27.007 tables them: RSRP index n for
// [-141 + n, -140 + n) dBm.
static void cmd_cesq(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;

    (void)a;
    if (mode == MODEM_TEST) {
        modem_answer(m, "+CESQ: (0-63,99),(0-7,99),(0-96,255),(0-49,255),(0-34,255),(0-97,255)");
        return;
    }
    if (mode != MODEM_EXEC) {
        modem_error(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+CESQ: 99,99,255,255,");
    if (!modem_registered(m) || m->set.rssi == MODEM_RSSI_UNKNOWN) {
        text_str(&t, "255,255");
    } else {
        int32_t rsrp = -113 + 2 * (int32_t)m->set.rssi - 10;
        text_str(&t, "20,");
        text_u32(&t, (uint32_t)(rsrp + 141));
    }
    modem_answer_text(m, &t);
}

static void set_apn(struct a7672x *s, const char *apn) {
    size_t n = text_strnlen(apn, A7672X_APN_MAX - 1);

    memcpy(s->ses.apn, apn, n);
    s->ses.apn[n] = '\0';
}

// Only context 1 is modelled; the others are accepted and not kept.
static void cmd_cgdcont(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;
    int32_t cid;

    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+CGDCONT: 1,\"IP\",\"");
        text_str(&t, s->ses.apn);
        text_char(&t, '"');
        modem_answer_text(m, &t);
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CGDCONT: (1-15),\"IP\",,,(0-2),(0-1),(0-1),(0-2)");
    } else if (mode == MODEM_EXEC) {
        modem_ok(m);
    } else if (!modem_arg_int(a, 0, 1, 15, &cid)) {
        modem_error(m);
    } else {
        if (cid == 1) {
            set_apn(s, a->count >= 3 ? a->v[2].str : "");
        }
        modem_ok(m);
    }
}

// Manual 5.2.4. Deactivating context 1 ends every link and session.
static void cmd_cgact(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    int32_t state;
    int32_t cid = 1;

    if (mode == MODEM_READ) {
        modem_answer(m, s->net.ses.bearer ? "+CGACT: 1,1" : "+CGACT: 1,0");
        return;
    }
    if (mode == MODEM_TEST) {
        modem_answer(m, "+CGACT: (0,1)");
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 1, &state) ||
        (a->count > 1 && !modem_arg_int(a, 1, 1, 15, &cid))) {
        modem_error(m);
        return;
    }
    if (cid != 1) {
        modem_ok(m);
    } else if (state == 1) {
        if (a7672x_activate(s)) {
            modem_ok(m);
        } else {
            modem_cme(m, MODEM_CME_NO_NETWORK);
        }
    } else {
        if (s->net.ses.bearer) {
            s->ses.pdp_at = false;
            s->ses.pdp_ppp = false;
            s->net.ses.bearer = false;
            modem_link_bearer_lost(m);
            a7672x_drop_sockets(s);
        }
        modem_ok(m);
    }
}

// Manual 5.2.13: the address without quotes, 0.0.0.0 while inactive.
static void cmd_cgpaddr(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    static const uint8_t none[4] = { 0, 0, 0, 0 };
    struct a7672x *s = a7672x_of(m);
    struct text t;
    int32_t cid = 1;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CGPADDR: (1)");
        return;
    }
    if ((mode != MODEM_EXEC && mode != MODEM_WRITE) ||
        (mode == MODEM_WRITE && !modem_arg_int(a, 0, 1, 15, &cid))) {
        modem_error(m);
        return;
    }
    if (cid != 1) {
        modem_ok(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+CGPADDR: 1,");
    text_ip(&t, s->net.ses.bearer ? s->net.set.ip : none);
    modem_answer_text(m, &t);
}

static uint8_t access_technology(const struct modem *m) {
    (void)m;
    return 7;
}

static void cmd_csclk(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    static const uint8_t allowed[] = { 0, 1, 2 };
    struct a7672x *s = a7672x_of(m);

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CSCLK: (0-2)");
    } else if (mode == MODEM_EXEC) {
        s->ses.csclk = 0;
        modem_ok(m);
    } else {
        modem_small_setting(m, mode, a, "+CSCLK: ", &s->ses.csclk, allowed, sizeof(allowed));
    }
}

// Manual 11.2.12: the supply voltage alone, in volts with three decimals.
static void cmd_cbc(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;

    (void)a;
    if (mode != MODEM_EXEC) {
        modem_error(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+CBC: ");
    text_fixed(&t, m->set.battery_mv, 3);
    text_char(&t, 'V');
    modem_answer_text(m, &t);
}

static void cmd_cpmutemp(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;

    (void)a;
    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode != MODEM_EXEC) {
        modem_error(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+CPMUTEMP: ");
    text_i32(&t, a7672x_of(m)->set.temperature);
    modem_answer_text(m, &t);
}

// Manual 4.2.3. The fake network offers no USSD service, so a request is
// answered with <m> 4, operation not supported.
static void cmd_cusd(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    int32_t n;

    if (mode == MODEM_READ) {
        modem_answer_u32(m, "+CUSD: ", s->ses.cusd_n);
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CUSD: (0-2)");
    } else if (mode == MODEM_EXEC) {
        s->ses.cusd_n = 0;
        modem_ok(m);
    } else if (!modem_arg_int(a, 0, 0, 2, &n)) {
        modem_error(m);
    } else {
        if (n != 2) {
            s->ses.cusd_n = (uint8_t)n;
        }
        modem_ok(m);
        if (a->count > 1 && n != 2) {
            modem_event(m, A7672X_REPLY_DELAY_US, A7672X_EV_CUSD, A7672X_CUSD_NOT_SUPPORTED);
        }
    }
}

// Manual 38.2.1: OK, then the fix, latitude first, with a 4-digit year.
// The fake base-station fix is the latitude and longitude properties,
// available whenever the modem is attached.
static void cmd_clbs(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t type;
    int32_t cid;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CLBS: (1,2,3,4,9),(1-15),(-180.000000-180.000000),(-90.000000-90.000000),(0-2)");
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 1, 4, &type) || (type != 1 && type != 4) ||
        (a->count > 1 && a->v[1].str[0] != '\0' && !modem_arg_int(a, 1, 1, 15, &cid))) {
        modem_error(m);
        return;
    }
    modem_ok(m);
    modem_event(m, A7672X_REPLY_DELAY_US, A7672X_EV_CLBS,
        (uint8_t)(modem_attached(m) ? type : A7672X_CLBS_FAILED));
}

// Manual 14.2.5: the write form sets the server and zone (default 32),
// the execution form syncs and reports. A successful sync applies the
// zone when the engine's clock can carry it.
static void cmd_cntp(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct a7672x *s = a7672x_of(m);
    struct text t;
    int32_t tz = A7672X_NTP_DEFAULT_TZ;

    if (mode == MODEM_WRITE) {
        if (a->count < 1 || a->v[0].str[0] == '\0' ||
            (a->count > 1 && !modem_arg_int(a, 1, -A7672X_NTP_TZ_MAX, A7672X_NTP_TZ_MAX, &tz))) {
            modem_error(m);
            return;
        }
        s->ses.ntp_tz = (int8_t)tz;
        modem_ok(m);
    } else if (mode == MODEM_EXEC) {
        modem_ok(m);
        modem_event(m, A7672X_REPLY_DELAY_US, A7672X_EV_CNTP, s->net.ses.bearer ? A7672X_NTP_OK : A7672X_NTP_NETWORK);
    } else if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+CNTP: \"\",");
        text_i32(&t, s->ses.ntp_tz);
        modem_answer_text(m, &t);
    } else {
        modem_answer(m, "+CNTP: \"HOST\",(-96~96)");
    }
}

static void clbs_event(struct modem *m, uint8_t arg) {
    const struct a7672x_settings *set = &a7672x_of(m)->set;
    const int32_t *time = m->set.time;
    struct text t;

    text_init(&t);
    text_str(&t, "+CLBS: ");
    if ((arg & A7672X_CLBS_FAILED) != 0) {
        text_u32(&t, A7672X_CLBS_NONET);
        modem_info(m, &t);
        return;
    }
    text_str(&t, "0,");
    text_fixed(&t, set->latitude, 6);
    text_char(&t, ',');
    text_fixed(&t, set->longitude, 6);
    text_char(&t, ',');
    text_u32(&t, A7672X_CLBS_ACCURACY_M);
    if (arg == 4) {
        text_char(&t, ',');
        text_u32(&t, (uint32_t)time[0]);
        text_char(&t, '/');
        text_d2(&t, (uint32_t)time[1]);
        text_char(&t, '/');
        text_d2(&t, (uint32_t)time[2]);
        text_char(&t, ',');
        text_d2(&t, (uint32_t)time[3]);
        text_char(&t, ':');
        text_d2(&t, (uint32_t)time[4]);
        text_char(&t, ':');
        text_d2(&t, (uint32_t)time[5]);
    }
    modem_info(m, &t);
}

static void event(struct modem *m, uint8_t kind, uint8_t arg) {
    struct a7672x *s = a7672x_of(m);
    struct text t;

    text_init(&t);
    switch (kind) {
        case A7672X_EV_CNTP:
            text_str(&t, "+CNTP: ");
            text_u32(&t, arg);
            modem_info(m, &t);
            if (arg == A7672X_NTP_OK && s->ses.ntp_tz >= MODEM_TIMEZONE_MIN && s->ses.ntp_tz <= MODEM_TIMEZONE_MAX) {
                m->set.timezone = s->ses.ntp_tz;
            }
            return;
        case A7672X_EV_CUSD:
            text_str(&t, "+CUSD: ");
            text_u32(&t, arg);
            modem_info(m, &t);
            return;
        case A7672X_EV_CLBS:
            clbs_event(m, arg);
            return;
        case A7672X_EV_CCHSTART:
        case A7672X_EV_CCHSTOP:
        case A7672X_EV_CCH_STOPPED:
        case A7672X_EV_CCHOPEN:
        case A7672X_EV_CCHSEND:
        case A7672X_EV_CCHEVENT:
        case A7672X_EV_CCH_PEER_CLOSED:
        case A7672X_EV_CCHCLOSE:
            a7672x_ssl_event(s, kind, arg);
            return;
        default:
            a7672x_socket_event(s, kind, arg);
            return;
    }
}

static void bearer_lost(struct modem *m) {
    struct a7672x *s = a7672x_of(m);

    s->ses.pdp_at = false;
    s->ses.pdp_ppp = false;
    modem_link_bearer_lost(m);
    a7672x_drop_sockets(s);
}

// No manual or hardware design documents the boot output. Field units
// print these four, and TinyGSM and Zephyr's modem_cellular need none of
// them: modem_cellular waits out its 20 s startup unless it reads APP RDY,
// which a real A7672X does not send, so the model adds it only when
// ready_urc is set. Without a ready SIM the SMS and phonebook lines stay
// away.
static void booted(struct modem *m) {
    modem_out_str(m, "\r\n*ATREADY: 1\r\n");
    modem_line(m, modem_cpin_line(modem_sim_cpin(m)));
    if (modem_sim_ready(m)) {
        modem_out_str(m, "\r\nSMS DONE\r\n");
        modem_out_str(m, "\r\nPB DONE\r\n");
    }
    if (a7672x_of(m)->set.ready_urc) {
        modem_out_str(m, "\r\nAPP RDY\r\n");
    }
}

static void session(struct modem *m) {
    struct a7672x_session *ses = &a7672x_of(m)->ses;

    memset(ses, 0, sizeof(*ses));
    ses->ntp_tz = A7672X_NTP_DEFAULT_TZ;
    for (size_t i = 0; i < A7672X_SESSIONS; i++) {
        ses->cch[i].ssl_ctx = 0xff;
    }
    // Manual 19.2.1: <sslversion> 4 accepts every protocol by default.
    memset(ses->sslversion, 4, sizeof(ses->sslversion));
}

static void defaults(struct modem *m) {
    struct a7672x_settings *set = &a7672x_of(m)->set;

    memset(set, 0, sizeof(*set));
    set->temperature = A7672X_DEFAULT_TEMPERATURE;
}

static int part_get(struct modem *m, size_t property, struct mclw_value *out) {
    const struct a7672x_settings *set = &a7672x_of(m)->set;

    switch (property) {
        case PROP_LATITUDE:
            out->v[0] = set->latitude;
            return 0;
        case PROP_LONGITUDE:
            out->v[0] = set->longitude;
            return 0;
        case PROP_TEMPERATURE:
            out->v[0] = set->temperature;
            return 0;
        case PROP_READY_URC:
            out->v[0] = set->ready_urc;
            return 0;
        default:
            return -EINVAL;
    }
}

static int part_set(struct modem *m, size_t property, const struct mclw_value *in) {
    struct a7672x_settings *set = &a7672x_of(m)->set;

    switch (property) {
        case PROP_LATITUDE:
            set->latitude = (int32_t)in->v[0];
            return 0;
        case PROP_LONGITUDE:
            set->longitude = (int32_t)in->v[0];
            return 0;
        case PROP_TEMPERATURE:
            set->temperature = (int16_t)in->v[0];
            return 0;
        case PROP_READY_URC:
            set->ready_urc = in->v[0] != 0;
            return 0;
        default:
            return -EINVAL;
    }
}

static const struct modem_part a7672x_part = {
    .property_count = PROP_COUNT,
    .region_count = 0,
    .defaults = defaults,
    .clear = NULL,
    .session = session,
    .get = part_get,
    .set = part_set,
    .region = NULL,
    .written = NULL,
};

static const struct modem_command commands[] = {
    { "+CFUN", cmd_cfun },
    { "+CRESET", cmd_creset },
    { "+CPOF", cmd_cpof },
    { "+CICCID", cmd_ciccid },
    { "+CSPN", cmd_cspn },
    { "+CESQ", cmd_cesq },
    { "+CGDCONT", cmd_cgdcont },
    { "+CGACT", cmd_cgact },
    { "+CGPADDR", cmd_cgpaddr },
    { "+CSCLK", cmd_csclk },
    { "+IFC", modem_cmd_accept },
    { "+ICF", modem_cmd_accept },
    { "+CBC", cmd_cbc },
    { "+CPMUTEMP", cmd_cpmutemp },
    { "+CUSD", cmd_cusd },
    { "+CLBS", cmd_clbs },
    { "+CNTP", cmd_cntp },
    { "+CNTPCID", cmd_absent },
    { "+CGNSSPWR", cmd_absent },
    { "+CMUX", modem_cmd_cmux },
    { "D", modem_cmd_dial_ppp },
};

static const struct modem_commands own_commands = MODEM_COMMANDS(commands);

static const struct modem_commands *const tables[] = {
    &own_commands,
    &a7672x_socket_commands,
    &a7672x_ssl_commands,
    &sms_commands,
    &modem_standard_commands,
};

static const struct mclw_property_desc properties[] = {
    MODEM_PROPERTIES,
    NET_PROPERTIES,
    PPP_PROPERTIES,
    SMS_PROPERTIES,
    MODEM_PROP("latitude", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "1e-6 deg", -90000000, 90000000),
    MODEM_PROP("longitude", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "1e-6 deg", -180000000, 180000000),
    MODEM_PROP("temperature", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "degC", A7672X_TEMPERATURE_MIN,
        A7672X_TEMPERATURE_MAX),
    MODEM_PROP_BOOL("ready_urc", MCLW_ACCESS_RW),
};

static const struct mclw_region_desc regions[] = {
    NET_REGIONS,
    SMS_REGIONS,
};

static const struct modem_part *const parts[] = {
    &modem_engine_part,
    &net_part,
    &ppp_part,
    &sms_part,
    &a7672x_part,
};

_Static_assert(sizeof(properties) / sizeof(properties[0]) ==
        MODEM_PROPERTY_COUNT + NET_PROPERTY_COUNT + PPP_PROPERTY_COUNT + SMS_PROPERTY_COUNT + PROP_COUNT,
    "the property table and the parts disagree");
_Static_assert(sizeof(regions) / sizeof(regions[0]) == NET_REGION_COUNT + SMS_REGION_COUNT,
    "the region table and the parts disagree");

static const struct mclw_binding_desc bindings[] = MODEM_BINDINGS(A7672X_DEFAULT_BAUD);

// Manual 10.2.3 lists 300 to 3600000; the engine's UART range is 9600 to
// 921600.
static const uint32_t rates[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };

static const struct modem_config config = {
    .manufacturer = A7672X_MANUFACTURER,
    .model = A7672X_MODEL,
    .revision = "+CGMR: " A7672X_FIRMWARE,
    // Manual 2.2.7; Cat-1 modules print no +GCAP line.
    .product = "Manufacturer: INCORPORATED\r\nModel: " A7672X_MODEL "\r\nRevision: " A7672X_FIRMWARE
               "\r\nIMEI: " A7672X_IMEI,
    .imei = A7672X_IMEI,
    .imsi = A7672X_IMSI,
    .iccid = A7672X_ICCID,
    .operator_name = A7672X_OPERATOR,
    .rates = rates,
    .rate_count = sizeof(rates) / sizeof(rates[0]),
    .ipr_test = "+IPR: (9600,19200,38400,57600,115200,230400,460800,921600)",
    .cfun_levels = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7),
    .cfun_reboot = 1u << 6,
    .cfun_test = "+CFUN: (0-1,4-7),(0-1)",
    .pwrkey_on_us = A7672X_PWRKEY_ON_US,
    .pwrkey_off_us = A7672X_PWRKEY_OFF_US,
    .boot_ms = A7672X_DEFAULT_BOOT_MS,
    // Manual 3.2.5, spelled as the module spells it.
    .power_down = "POWERD DOWN",
    .command_room = A7672X_COMMAND_ROOM,
    .urc_room = A7672X_URC_ROOM,
    .tables = tables,
    .table_count = sizeof(tables) / sizeof(tables[0]),
    .parts = parts,
    .part_count = sizeof(parts) / sizeof(parts[0]),
    .booted = booted,
    .event = event,
    .bearer_lost = bearer_lost,
    .access_technology = access_technology,
    .link_bearer = link_bearer,
};

static int a7672x_init(struct mclw_instance *inst, const struct mclw_binding *bindings_in) {
    struct a7672x *s = inst->state;

    s->m.net = &s->net;
    s->m.sms = &s->sms;
    s->m.gnss = NULL;
    s->m.link = &s->link;
    net_init(&s->net, inst, s->sockets, A7672X_SOCKETS, a7672x_net_notify, s);
    return modem_init(&s->m, inst, bindings_in, s->events, A7672X_EVENTS);
}

static const struct mclw_model_ops ops = {
    .init = a7672x_init,
    .deinit = modem_deinit,
    .reset = modem_reset,
    .get = modem_get,
    .set = modem_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = modem_read_region,
    .write_region = modem_write_region,
    .time_advanced = modem_time_advanced,
    .usb_rx = NULL,
};

const struct mclw_model mclw_model_a7672x = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "a7672x",
    .family = "cellular",
    .config = &config,
    .properties = properties,
    .property_count = sizeof(properties) / sizeof(properties[0]),
    .regions = regions,
    .region_count = sizeof(regions) / sizeof(regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = bindings,
    .binding_count = sizeof(bindings) / sizeof(bindings[0]),
    .state_size = sizeof(struct a7672x),
    .ops = &ops,
};
