// SPDX-License-Identifier: Apache-2.0 OR MIT
// The SIM7000 model: its descriptors and identity, boot, the data bearer
// (SAPBR, CGACT and CNACT beside the CIP stack's own), GNSS, base-station
// location, NTP and USSD.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/version.h"

#include "gnss.h"
#include "modem.h"
#include "net.h"
#include "sim7000.h"
#include "sms.h"
#include "text.h"

struct sim7000 *sim7000_of(struct modem *m) {
    return (struct sim7000 *)m;
}

bool sim7000_arg_int(const struct modem_args *a, uint8_t i, int32_t lo, int32_t hi, int32_t *out) {
    if (i < a->count && a->v[i].quoted) {
        const char *p = a->v[i].str;
        int32_t v = 0;

        if (*p == '\0') {
            return false;
        }
        for (; *p >= '0' && *p <= '9'; p++) {
            v = v * 10 + (*p - '0');
            if (v > 99999999) {
                return false;
            }
        }
        if (*p != '\0' || v < lo || v > hi) {
            return false;
        }
        *out = v;
        return true;
    }
    return modem_arg_int(a, i, lo, hi, out);
}

void sim7000_bearer(struct sim7000 *s, uint8_t holder, bool up) {
    if (up) {
        s->ses.bearers = (uint8_t)(s->ses.bearers | holder);
    } else {
        s->ses.bearers = (uint8_t)(s->ses.bearers & ~holder);
    }
    s->net.ses.bearer = s->ses.bearers != 0;
}

void sim7000_set_apn(struct sim7000 *s, const char *apn) {
    size_t n = text_strnlen(apn, SIM7000_APN_MAX - 1);

    memcpy(s->ses.apn, apn, n);
    s->ses.apn[n] = '\0';
}

// Manual 5.2.1: 1 sends NORMAL POWER DOWN, 0 powers off urgently.
static void cmd_cpowd(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t n;

    if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, 1, &n)) {
        modem_power_off(m, n == 1);
    } else {
        modem_error(m);
    }
}

static uint8_t access_technology(const struct modem *m) {
    (void)m;
    return MODEM_ACT_CATM;
}

static void cmd_cgdcont(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;
    int32_t cid;

    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+CGDCONT: 1,\"IP\",\"");
        text_str(&t, s->ses.apn);
        text_str(&t, "\",\"0.0.0.0\",0,0");
        modem_answer_text(m, &t);
        return;
    }
    if (mode == MODEM_WRITE && a->count >= 3 && modem_arg_int(a, 0, 1, 15, &cid)) {
        if (cid == 1) {
            sim7000_set_apn(s, a->v[2].str);
        }
        modem_ok(m);
    } else if (mode == MODEM_WRITE) {
        modem_error(m);
    } else {
        modem_ok(m);
    }
}

// Manual 5.2.31: the CNACT stack's PDP configuration, `<ip_type>[,<APN>...]`
// with no context number.
static void cmd_cncfg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;
    int32_t type;

    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+CNCFG: 1,\"");
        text_str(&t, s->ses.apn);
        text_str(&t, "\",\"\",\"\",0");
        modem_answer_text(m, &t);
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CNCFG: (0-2),32,32,32,(0-3)");
    } else if (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, 2, &type)) {
        if (a->count >= 2) {
            sim7000_set_apn(s, a->v[1].str);
        }
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

// Manual 6.2.3: context 1 only; the state follows the attach.
static void cmd_cgact(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    int32_t state;
    int32_t cid = 1;

    if (mode == MODEM_READ) {
        modem_answer(m, s->ses.cgact ? "+CGACT: 1,1" : "+CGACT: 1,0");
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CGACT: (0,1)");
    } else if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 1, &state) ||
        (a->count > 1 && !modem_arg_int(a, 1, 1, 24, &cid))) {
        modem_error(m);
    } else if (state == 1 && !modem_attached(m)) {
        modem_cme(m, MODEM_CME_NO_NETWORK);
    } else {
        if (cid == 1) {
            s->ses.cgact = state == 1;
        }
        modem_ok(m);
    }
}

// Manual 7.2.1: profile 1 is the bearer HTTP, FTP, NTP and location use.
static void cmd_sapbr(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;
    int32_t cmd;
    int32_t cid;
    bool open = (s->ses.bearers & SIM7000_BEARER_SAPBR) != 0;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+SAPBR: (0-4),(1-3),\"ConParamTag\",\"ConParamValue\"");
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 4, &cmd) || !modem_arg_int(a, 1, 1, 3, &cid)) {
        modem_error(m);
        return;
    }
    if (cmd == 3 && a->count >= 4) {
        if (text_equal_nocase(a->v[2].str, "APN")) {
            sim7000_set_apn(s, a->v[3].str);
        }
        modem_ok(m);
    } else if (cmd == 4 && a->count >= 3) {
        bool apn = text_equal_nocase(a->v[2].str, "APN");
        text_init(&t);
        text_str(&t, "+SAPBR: \"");
        text_str(&t, a->v[2].str);
        text_str(&t, "\",\"");
        text_str(&t, apn ? s->ses.apn : text_equal_nocase(a->v[2].str, "Contype") ? "GPRS" : "");
        text_char(&t, '"');
        modem_answer_text(m, &t);
    } else if (cmd == 2) {
        bool up = open && cid == SIM7000_SAPBR_CID;
        text_init(&t);
        text_str(&t, "+SAPBR: ");
        text_u32(&t, (uint32_t)cid);
        text_str(&t, up ? ",1,\"" : ",3,\"");
        text_ip(&t, up ? s->net.set.ip : (const uint8_t[4]){ 0, 0, 0, 0 });
        text_char(&t, '"');
        modem_answer_text(m, &t);
    } else if (cmd == 1 && cid == SIM7000_SAPBR_CID && !open) {
        if (!modem_attached(m)) {
            modem_cme(m, MODEM_CME_NO_NETWORK);
            return;
        }
        sim7000_bearer(s, SIM7000_BEARER_SAPBR, true);
        modem_ok(m);
    } else if (cmd == 0 && cid == SIM7000_SAPBR_CID && open) {
        sim7000_bearer(s, SIM7000_BEARER_SAPBR, false);
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

// Manual 5.2.30: `+CNACT=<mode>[,<apn>]`; OK first, then +APP PDP once the
// application network is up or down, with no context number. A failed
// activation is the DEACTIVE URC after the OK.
static void cmd_cnact(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    struct text t;
    int32_t action;
    bool up = (s->ses.bearers & SIM7000_BEARER_CNACT) != 0;

    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, up ? "+CNACT: 1,\"" : "+CNACT: 0,\"");
        text_ip(&t, up ? s->net.set.ip : (const uint8_t[4]){ 0, 0, 0, 0 });
        text_char(&t, '"');
        modem_answer_text(m, &t);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_answer(m, "+CNACT: (0-2)");
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 2, &action)) {
        modem_error(m);
        return;
    }
    if (action == 0) {
        modem_ok(m);
        if (up) {
            sim7000_bearer(s, SIM7000_BEARER_CNACT, false);
            modem_event(m, 0, SIM7000_EV_APP_PDP, 0);
            sim7000_drop_sockets(s, SIM7000_OWNER_CA);
        }
    } else if (up) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
    } else {
        if (a->count > 1) {
            sim7000_set_apn(s, a->v[1].str);
        }
        modem_ok(m);
        if (modem_attached(m)) {
            sim7000_bearer(s, SIM7000_BEARER_CNACT, true);
        }
        modem_event(m, 0, SIM7000_EV_APP_PDP, modem_attached(m) ? 1 : 0);
    }
}

static void cmd_cgnspwr(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct gnss *g = m->gnss;
    int32_t v;

    if (mode == MODEM_READ) {
        modem_answer(m, g->on ? "+CGNSPWR: 1" : "+CGNSPWR: 0");
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CGNSPWR: (0,1)");
    } else if (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, 1, &v)) {
        g->on = v == 1;
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

static void text_utc(struct text *t, const int32_t *time) {
    text_u32(t, (uint32_t)time[0]);
    for (size_t i = 1; i < 6; i++) {
        text_d2(t, (uint32_t)time[i]);
    }
    text_str(t, ".000");
}

// A field is written after the comma that separates it from the previous one.
static void comma(struct text *t, size_t count) {
    for (size_t i = 0; i < count; i++) {
        text_char(t, ',');
    }
}

// Manual table 15-1: 21 fields. PDOP and VDOP repeat HDOP; HPA and VPA are
// five times HDOP in metres; the strongest C/N0 is a fixed 40 dBHz.
static void cmd_cgnsinf(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    const struct gnss *g = m->gnss;
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
    text_str(&t, "+CGNSINF: ");
    if (!g->on) {
        text_char(&t, '0');
        comma(&t, 20);
    } else if (!g->fix) {
        text_str(&t, "1,0,");
        text_utc(&t, m->set.time);
        comma(&t, 6);
        text_char(&t, '0');
        comma(&t, 6);
        text_u32(&t, g->satellites);
        comma(&t, 6);
    } else {
        text_str(&t, "1,1,");
        text_utc(&t, m->set.time);
        text_char(&t, ',');
        text_fixed(&t, g->latitude, 6);
        text_char(&t, ',');
        text_fixed(&t, g->longitude, 6);
        text_char(&t, ',');
        text_fixed(&t, g->altitude, 3);
        text_char(&t, ',');
        text_fixed(&t, (int32_t)g->speed, 2);
        text_char(&t, ',');
        text_fixed(&t, g->course, 1);
        text_str(&t, ",1,");
        for (size_t i = 0; i < 3; i++) {
            text_char(&t, ',');
            text_fixed(&t, g->hdop, 1);
        }
        text_str(&t, ",,");
        text_u32(&t, g->satellites);
        text_char(&t, ',');
        text_u32(&t, g->satellites);
        text_str(&t, ",0,,40,");
        text_fixed(&t, g->hdop * 5, 1);
        text_char(&t, ',');
        text_fixed(&t, g->hdop * 5, 1);
    }
    modem_answer_text(m, &t);
}

// Not in the SIM7000 manual; TinyGSM's GSM location takes the SIM7080's
// form: longitude before latitude, the GNSS position as the fake cell's.
static void cmd_clbs(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;
    int32_t type;
    int32_t cid;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CLBS: (1,4),(0-3)");
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 1, 4, &type) || (type != 1 && type != 4) ||
        !modem_arg_int(a, 1, 0, 3, &cid)) {
        modem_error(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+CLBS: ");
    if (!modem_attached(m)) {
        text_u32(&t, SIM7000_CLBS_NET_ERROR);
        modem_answer_text(m, &t);
        return;
    }
    text_u32(&t, SIM7000_CLBS_OK);
    text_char(&t, ',');
    text_fixed(&t, m->gnss->longitude, 6);
    text_char(&t, ',');
    text_fixed(&t, m->gnss->latitude, 6);
    text_char(&t, ',');
    text_u32(&t, SIM7000_CLBS_ACCURACY_M);
    if (type == 4) {
        text_char(&t, ',');
        text_date_time(&t, m->set.time);
    }
    modem_answer_text(m, &t);
}

// Manual 11.2.2: the write form configures, the execution form syncs and
// reports with a URC. A successful sync applies the configured zone. Syncing
// needs the network, not a particular bearer: TinyGSM's SIM7000SSL driver
// never opens SAPBR profile 1.
static void cmd_cntp(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7000 *s = sim7000_of(m);
    int32_t tz;

    if (mode == MODEM_WRITE) {
        if (a->count < 1 || a->v[0].str[0] == '\0' || (a->count > 1 && !modem_arg_int(a, 1, -47, 48, &tz))) {
            modem_error(m);
            return;
        }
        s->ses.ntp_tz_set = a->count > 1;
        s->ses.ntp_tz = a->count > 1 ? (int8_t)tz : 0;
        modem_ok(m);
    } else if (mode == MODEM_EXEC) {
        modem_ok(m);
        modem_event(m, SIM7000_URC_DELAY_US, SIM7000_EV_NTP,
            modem_attached(m) ? SIM7000_CNTP_OK : SIM7000_CNTP_NETWORK_ERROR);
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CNTP: 64,(-47-48),(1-3),(0-2)");
    } else {
        modem_ok(m);
    }
}

// Manual 3.2.23: there is no USSD service, so the answer is `+CUSD: 4`.
static void cmd_cusd(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t n;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CUSD: (0-2)");
    } else if (mode == MODEM_READ) {
        modem_answer(m, "+CUSD: 0");
    } else if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 2, &n)) {
        modem_error(m);
    } else if (!modem_registered(m)) {
        modem_cme(m, MODEM_CME_NO_NETWORK);
    } else {
        modem_ok(m);
        if (n == 1 && a->count > 1) {
            modem_event(m, SIM7000_URC_DELAY_US, SIM7000_EV_CUSD, 0);
        }
    }
}

static void event(struct modem *m, uint8_t kind, uint8_t arg) {
    struct text t;

    text_init(&t);
    switch (kind) {
        case SIM7000_EV_APP_PDP:
            modem_info_str(m, arg == 1 ? "+APP PDP: ACTIVE" : "+APP PDP: DEACTIVE");
            return;
        case SIM7000_EV_NTP:
            text_str(&t, "+CNTP: ");
            text_u32(&t, arg);
            modem_info(m, &t);
            if (arg == SIM7000_CNTP_OK && sim7000_of(m)->ses.ntp_tz_set) {
                m->set.timezone = sim7000_of(m)->ses.ntp_tz;
            }
            return;
        case SIM7000_EV_CUSD:
            modem_info_str(m, "+CUSD: 4");
            return;
        case SIM7000_EV_PDP_DEACT:
            modem_info_str(m, "+PDP: DEACT");
            return;
        case SIM7000_EV_SAPBR_DEACT:
            modem_info_str(m, "+SAPBR 1: DEACT");
            return;
        default:
            sim7000_socket_event(sim7000_of(m), kind, arg);
            return;
    }
}

// The network is gone: every holder of the bearer reports it in its own
// form (manual 8.2.7 +PDP: DEACT, 5.2.30 +APP PDP, 7.2.1 +SAPBR), the CIP
// stack stays in PDP DEACT until AT+CIPSHUT, and every connection ends.
static void bearer_lost(struct modem *m) {
    struct sim7000 *s = sim7000_of(m);
    uint8_t held = s->ses.bearers;

    s->ses.bearers = 0;
    s->ses.cgact = false;
    if ((held & SIM7000_BEARER_CIP) != 0) {
        s->ses.cip_state = SIM7000_CIP_PDP_DEACT;
        modem_event(m, 0, SIM7000_EV_PDP_DEACT, 0);
    }
    if ((held & SIM7000_BEARER_CNACT) != 0) {
        modem_event(m, 0, SIM7000_EV_APP_PDP, 0);
    }
    if ((held & SIM7000_BEARER_SAPBR) != 0) {
        modem_event(m, 0, SIM7000_EV_SAPBR_DEACT, 0);
    }
    sim7000_drop_sockets(s, SIM7000_OWNER_NONE);
}

// Manual 8.2 URC list and the TinyGSM spec: RDY, +CFUN: 1, +CPIN and SMS
// Ready, in that order. SMS Ready is not in the manual, but TinyGSM's restart
// waits 30 s for it and re-runs its init on it.
static void booted(struct modem *m) {
    modem_out_str(m, "\r\nRDY\r\n");
    modem_out_str(m, "\r\n+CFUN: 1\r\n");
    modem_line(m, modem_cpin_line(modem_sim_cpin(m)));
    modem_out_str(m, "\r\nSMS Ready\r\n");
}

static void session(struct modem *m) {
    struct sim7000_session *ses = &sim7000_of(m)->ses;

    memset(ses, 0, sizeof(*ses));
    ses->sslversion = 3;
}

static const struct modem_part sim7000_part = {
    .property_count = 0,
    .region_count = 0,
    .defaults = NULL,
    .clear = NULL,
    .session = session,
    .get = NULL,
    .set = NULL,
    .region = NULL,
    .written = NULL,
};

static const struct modem_command commands[] = {
    { "+CPOWD", cmd_cpowd },
    { "+CLTS", modem_cmd_accept },
    { "+CBATCHK", modem_cmd_accept },
    { "+CSCLK", modem_cmd_accept },
    { "+CGDCONT", cmd_cgdcont },
    { "+CNCFG", cmd_cncfg },
    { "+CGACT", cmd_cgact },
    { "+SAPBR", cmd_sapbr },
    { "+CNACT", cmd_cnact },
    { "+CGNSPWR", cmd_cgnspwr },
    { "+CGNSCOLD", modem_cmd_accept },
    { "+CGNSWARM", modem_cmd_accept },
    { "+CGNSHOT", modem_cmd_accept },
    { "+CGNSINF", cmd_cgnsinf },
    { "+CLBS", cmd_clbs },
    { "+CUSD", cmd_cusd },
    { "+CNTPCID", modem_cmd_accept },
    { "+CNTP", cmd_cntp },
};

static const struct modem_commands own_commands = MODEM_COMMANDS(commands);

static const struct modem_commands *const tables[] = {
    &own_commands,
    &sim7000_cip_commands,
    &sim7000_ca_commands,
    &sms_commands,
    &modem_standard_commands,
};

static const struct mclw_property_desc properties[] = {
    MODEM_PROPERTIES,
    NET_PROPERTIES,
    GNSS_PROPERTIES,
    SMS_PROPERTIES,
};

static const struct mclw_region_desc regions[] = {
    NET_REGIONS,
    SMS_REGIONS,
};

static const struct modem_part *const parts[] = {
    &modem_engine_part,
    &net_part,
    &gnss_part,
    &sms_part,
    &sim7000_part,
};

_Static_assert(sizeof(properties) / sizeof(properties[0]) ==
        MODEM_PROPERTY_COUNT + NET_PROPERTY_COUNT + GNSS_PROPERTY_COUNT + SMS_PROPERTY_COUNT,
    "the property table and the parts disagree");
_Static_assert(sizeof(regions) / sizeof(regions[0]) == NET_REGION_COUNT + SMS_REGION_COUNT,
    "the region table and the parts disagree");

static const struct mclw_binding_desc bindings[] = MODEM_BINDINGS(SIM7000_DEFAULT_BAUD);

// Manual 2.2.32 rates; autobauding (0) and the rates above 921600 are not
// modelled.
static const uint32_t rates[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };

static const struct modem_config config = {
    .manufacturer = SIM7000_MANUFACTURER,
    .model = SIM7000_MODEL,
    .revision = SIM7000_REVISION,
    .product = SIM7000_PRODUCT,
    .imei = SIM7000_IMEI,
    .imsi = SIM7000_IMSI,
    .iccid = SIM7000_ICCID,
    .operator_name = SIM7000_OPERATOR,
    .rates = rates,
    .rate_count = sizeof(rates) / sizeof(rates[0]),
    .ipr_test = "+IPR: (),(9600,19200,38400,57600,115200,230400,460800,921600)",
    // Manual 3.2.19: 0, 1, 4, 5, 6 (reset) and 7.
    .cfun_levels = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7),
    .cfun_reboot = 1u << 6,
    .cfun_test = "+CFUN: (0,1,4-7),(0,1)",
    .pwrkey_on_us = SIM7000_PWRKEY_ON_US,
    .pwrkey_off_us = SIM7000_PWRKEY_OFF_US,
    .boot_ms = SIM7000_DEFAULT_BOOT_MS,
    .power_down = "NORMAL POWER DOWN",
    .command_room = SIM7000_COMMAND_ROOM,
    .urc_room = SIM7000_URC_ROOM,
    .tables = tables,
    .table_count = sizeof(tables) / sizeof(tables[0]),
    .parts = parts,
    .part_count = sizeof(parts) / sizeof(parts[0]),
    .booted = booted,
    .event = event,
    .bearer_lost = bearer_lost,
    .access_technology = access_technology,
};

static int sim7000_init(struct mclw_instance *inst, const struct mclw_binding *bindings_in) {
    struct sim7000 *s = inst->state;

    s->m.net = &s->net;
    s->m.sms = &s->sms;
    s->m.gnss = &s->gnss;
    net_init(&s->net, inst, s->sockets, SIM7000_SOCKETS, sim7000_net_notify, s);
    return modem_init(&s->m, inst, bindings_in, s->events, SIM7000_EVENTS);
}

static const struct mclw_model_ops ops = {
    .init = sim7000_init,
    .deinit = modem_deinit,
    .reset = modem_reset,
    .get = modem_get,
    .set = modem_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = modem_read_region,
    .write_region = modem_write_region,
    .time_advanced = modem_time_advanced,
    .poll = modem_poll,
    .usb_rx = NULL,
};

const struct mclw_model mclw_model_sim7000 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "sim7000",
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
    .state_size = sizeof(struct sim7000),
    .ops = &ops,
};
