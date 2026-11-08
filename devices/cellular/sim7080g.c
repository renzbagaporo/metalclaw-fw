// SPDX-License-Identifier: Apache-2.0 OR MIT
// The SIM7080G model: its descriptors and identity, boot, the PDP bearer
// (CNACT), radio settings, GNSS, base-station location, NTP and FTP.

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
#include "sim7080g.h"
#include "sms.h"
#include "text.h"

struct sim7080g *sim7080g_of(struct modem *m) {
    return (struct sim7080g *)m;
}

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

static void cmd_creboot(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    modem_ok(m);
    if (mode == MODEM_EXEC) {
        modem_power_on(m);
    }
}

static bool cat_m(const struct sim7080g *s) {
    return s->ses.cmnb != SIM7080G_CMNB_NBIOT;
}

static uint8_t access_technology(const struct modem *m) {
    return cat_m((const struct sim7080g *)m) ? MODEM_ACT_CATM : MODEM_ACT_NBIOT;
}

// Manual 5.2.19, the CAT-M/NB-IoT form. RSSI follows the rssi property the
// way +CSQ maps it (-113 dBm + 2 dB per step); the rest is a fixed cell.
static void cmd_cpsi(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;

    (void)a;
    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode != MODEM_READ) {
        modem_error(m);
        return;
    }
    if (!modem_registered(m)) {
        modem_answer(m, "+CPSI: NO SERVICE,Online");
        return;
    }
    int32_t rssi = m->set.rssi == MODEM_RSSI_UNKNOWN ? -113 : -113 + 2 * (int32_t)m->set.rssi;
    text_init(&t);
    text_str(&t, cat_m(sim7080g_of(m)) ? "+CPSI: LTE CAT-M1" : "+CPSI: LTE NB-IOT");
    text_str(&t, ",Online," SIM7080G_MCC_MNC ",0x0001,12345678,123,EUTRAN-BAND20,6290,0,0,-10,");
    text_i32(&t, rssi - 10);
    text_char(&t, ',');
    text_i32(&t, rssi);
    text_str(&t, ",13");
    modem_answer_text(m, &t);
}

static void cmd_cnmp(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    static const uint8_t allowed[] = { 2, 13, 38, 51 };

    modem_small_setting(m, mode, a, "+CNMP: ", &sim7080g_of(m)->ses.cnmp, allowed, sizeof(allowed));
}

static void cmd_cmnb(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    static const uint8_t allowed[] = { SIM7080G_CMNB_CATM, SIM7080G_CMNB_NBIOT, SIM7080G_CMNB_BOTH };

    modem_small_setting(m, mode, a, "+CMNB: ", &sim7080g_of(m)->ses.cmnb, allowed, sizeof(allowed));
}

static void set_apn(struct sim7080g *s, const char *apn) {
    size_t n = text_strnlen(apn, SIM7080G_APN_MAX - 1);

    memcpy(s->ses.apn, apn, n);
    s->ses.apn[n] = '\0';
}

static void cmd_cgdcont(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7080g *s = sim7080g_of(m);
    struct text t;

    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+CGDCONT: 1,\"IP\",\"");
        text_str(&t, s->ses.apn);
        text_str(&t, "\",\"0.0.0.0\",0,0,0,0");
        modem_answer_text(m, &t);
        return;
    }
    if (mode == MODEM_WRITE && a->count >= 3) {
        set_apn(s, a->v[2].str);
    }
    modem_ok(m);
}

static void cmd_cgnapn(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7080g *s = sim7080g_of(m);
    struct text t;

    (void)a;
    if (mode != MODEM_EXEC) {
        modem_ok(m);
        return;
    }
    text_init(&t);
    if (modem_attached(m) && s->ses.apn[0] != '\0') {
        text_str(&t, "+CGNAPN: 1,\"");
        text_str(&t, s->ses.apn);
        text_char(&t, '"');
    } else {
        text_str(&t, "+CGNAPN: 0,\"\"");
    }
    modem_answer_text(m, &t);
}

static void cmd_cncfg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7080g *s = sim7080g_of(m);
    struct text t;
    int32_t pdp;
    int32_t type;

    if (mode == MODEM_READ) {
        modem_out_str(m, "\r\n");
        for (uint32_t i = 0; i < SIM7080G_PDP_CONTEXTS; i++) {
            text_init(&t);
            text_str(&t, "+CNCFG: ");
            text_u32(&t, i);
            text_str(&t, ",0,\"");
            text_str(&t, i == 0 ? s->ses.apn : "");
            text_str(&t, "\",\"\",\"\",0");
            modem_info_line(m, &t);
        }
        modem_ok(m);
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CNCFG: (0-3),(0-4),150,127,127,(0-3)");
    } else if (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, SIM7080G_PDP_CONTEXTS - 1, &pdp) &&
        modem_arg_int(a, 1, 0, 4, &type)) {
        if (pdp == 0 && a->count >= 3) {
            set_apn(s, a->v[2].str);
        }
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

// Manual 7.2.1: OK first, then +APP PDP once the bearer is up or down.
// Only context 0 is modelled.
static void cmd_cnact(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7080g *s = sim7080g_of(m);
    struct text t;
    int32_t pdp;
    int32_t action;

    if (mode == MODEM_READ) {
        static const uint8_t none[4] = { 0, 0, 0, 0 };
        modem_out_str(m, "\r\n");
        for (uint32_t i = 0; i < SIM7080G_PDP_CONTEXTS; i++) {
            bool up = i == 0 && s->net.ses.bearer;
            text_init(&t);
            text_str(&t, "+CNACT: ");
            text_u32(&t, i);
            text_str(&t, up ? ",1,\"" : ",0,\"");
            text_ip(&t, up ? s->net.set.ip : none);
            text_char(&t, '"');
            modem_info_line(m, &t);
        }
        modem_ok(m);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_answer(m, "+CNACT: (0-3),(0-2)");
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, SIM7080G_PDP_CONTEXTS - 1, &pdp) ||
        !modem_arg_int(a, 1, 0, 2, &action)) {
        modem_error(m);
        return;
    }
    if (pdp != 0) {
        modem_cme(m, MODEM_CME_NOT_SUPPORTED);
    } else if (action != 0) {
        if (s->net.ses.bearer || s->ses.pdp_pending) {
            modem_cme(m, MODEM_CME_NOT_ALLOWED);
        } else if (!modem_attached(m)) {
            modem_cme(m, MODEM_CME_NO_NETWORK);
        } else {
            s->ses.pdp_pending = true;
            modem_ok(m);
            modem_event(m, SIM7080G_URC_DELAY_US, SIM7080G_EV_APP_PDP, 1);
        }
    } else if (!s->net.ses.bearer && !s->ses.pdp_pending) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
    } else {
        s->net.ses.bearer = false;
        s->ses.pdp_pending = false;
        modem_ok(m);
        modem_event(m, SIM7080G_URC_DELAY_US, SIM7080G_EV_APP_PDP, 0);
        sim7080g_drop_sockets(s, SIM7080G_URC_DELAY_US);
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

// Manual table 8-1: 18 fields. PDOP and VDOP repeat HDOP; HPA and VPA are
// five times HDOP in metres.
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
    if (!g->on) {
        text_str(&t, "+CGNSINF: 0,,,,,,,,,,,,,,,,,");
    } else if (!g->fix) {
        text_str(&t, "+CGNSINF: 1,0,");
        text_utc(&t, m->set.time);
        text_str(&t, ",,,,,,0,,,,,,");
        text_u32(&t, g->satellites);
        text_str(&t, ",,,");
    } else {
        text_str(&t, "+CGNSINF: 1,1,");
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
        text_str(&t, ",1,,");
        for (size_t i = 0; i < 3; i++) {
            text_fixed(&t, g->hdop, 1);
            text_char(&t, ',');
        }
        text_char(&t, ',');
        text_u32(&t, g->satellites);
        text_str(&t, ",,");
        text_fixed(&t, g->hdop * 5, 1);
        text_char(&t, ',');
        text_fixed(&t, g->hdop * 5, 1);
    }
    modem_answer_text(m, &t);
}

// Manual 20.2.1: longitude before latitude. The fake base-station fix is
// the GNSS position, available whenever the bearer is up.
static void cmd_clbs(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;
    int32_t type;
    int32_t cid;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CLBS: (1,4),(0-3)");
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 1, 4, &type) || (type != 1 && type != 4) ||
        !modem_arg_int(a, 1, 0, SIM7080G_PDP_CONTEXTS - 1, &cid)) {
        modem_error(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+CLBS: ");
    if (!m->net->ses.bearer) {
        text_u32(&t, SIM7080G_CLBS_NET_ERROR);
        modem_answer_text(m, &t);
        return;
    }
    text_u32(&t, SIM7080G_CLBS_OK);
    text_char(&t, ',');
    text_fixed(&t, m->gnss->longitude, 6);
    text_char(&t, ',');
    text_fixed(&t, m->gnss->latitude, 6);
    text_char(&t, ',');
    text_u32(&t, SIM7080G_CLBS_ACCURACY_M);
    if (type == 4) {
        text_char(&t, ',');
        text_date_time(&t, m->set.time);
    }
    modem_answer_text(m, &t);
}

// Manual 16.2.2: the write form configures, the execution form syncs and
// reports with a URC. A successful sync applies the configured zone.
static void cmd_cntp(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7080g *s = sim7080g_of(m);
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
        modem_event(m, SIM7080G_URC_DELAY_US, SIM7080G_EV_NTP,
            s->net.ses.bearer ? SIM7080G_CNTP_OK : SIM7080G_CNTP_NETWORK_ERROR);
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CNTP: (1-64),(-47-48),(0-3),(0-2)");
    } else {
        modem_ok(m);
    }
}

static void cmd_ftpgetname(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    if (mode == MODEM_WRITE && a->count >= 1) {
        net_ftp_name(m->net, a->v[0].str);
    }
    modem_ok(m);
}

static void cmd_ftpgetpath(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    if (mode == MODEM_WRITE && a->count >= 1) {
        net_ftp_path(m->net, a->v[0].str);
    }
    modem_ok(m);
}

// Manual 15.2.14: FTPGET=1 opens the session and reports with a URC;
// FTPGET=2,<n> reads at most n bytes, and +FTPGET: 1,0 follows the read
// that leaves nothing behind.
static void cmd_ftpget(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sim7080g *s = sim7080g_of(m);
    struct sim7080g_session *ses = &s->ses;
    struct text t;
    int32_t op;
    int32_t want;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+FTPGET: (1,2),(1-1460)");
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 1, 2, &op)) {
        modem_error(m);
        return;
    }
    if (op == 1) {
        ses->ftp_off = 0;
        ses->ftp_len = s->net.set.file_length;
        ses->ftp_state = SIM7080G_FTP_STARTING;
        modem_ok(m);
        modem_event(m, SIM7080G_URC_DELAY_US, SIM7080G_EV_FTPGET,
            s->net.ses.bearer ? SIM7080G_FTP_DATA : SIM7080G_FTP_NET_ERROR);
        return;
    }
    if (!modem_arg_int(a, 1, 1, INT32_MAX, &want) || ses->ftp_state != SIM7080G_FTP_ACTIVE) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    size_t room = modem_out_free(m);
    size_t left = (size_t)(ses->ftp_len - ses->ftp_off);
    size_t n = left;
    size_t overhead = sizeof("\r\n+FTPGET: 2,0000\r\n") + sizeof("\r\n\r\nOK\r\n");
    if (n > (size_t)want) {
        n = (size_t)want;
    }
    if (n + overhead > room) {
        n = room > overhead ? room - overhead : 0;
    }
    if (n == 0 && left > 0) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    text_init(&t);
    text_str(&t, "+FTPGET: 2,");
    text_u32(&t, (uint32_t)n);
    modem_info(m, &t);
    if (n > 0) {
        modem_out(m, &s->net.set.file[ses->ftp_off], n);
        modem_out_str(m, "\r\n");
        ses->ftp_off = (uint16_t)(ses->ftp_off + n);
        s->net.stats.bytes_down += (uint32_t)n;
        if (ses->ftp_off == ses->ftp_len) {
            modem_event(m, 0, SIM7080G_EV_FTPGET, SIM7080G_FTP_DONE);
        }
    }
    modem_ok(m);
}

static void event(struct modem *m, uint8_t kind, uint8_t arg) {
    struct sim7080g *s = sim7080g_of(m);
    struct sim7080g_session *ses = &s->ses;
    struct text t;

    text_init(&t);
    switch (kind) {
        case SIM7080G_EV_APP_PDP:
            if (arg == 1) {
                if (!ses->pdp_pending) {
                    return;
                }
                ses->pdp_pending = false;
                s->net.ses.bearer = modem_attached(m);
                modem_info_str(m, s->net.ses.bearer ? "+APP PDP: 0,ACTIVE" : "+APP PDP: 0,DEACTIVE");
            } else {
                modem_info_str(m, "+APP PDP: 0,DEACTIVE");
            }
            return;
        case SIM7080G_EV_DNS:
            text_str(&t, "+CDNSGIP: ");
            if (ses->dns_ok) {
                text_str(&t, "1,\"");
                text_str(&t, ses->dns_host);
                text_str(&t, "\",\"");
                text_ip(&t, ses->dns_ip);
                text_char(&t, '"');
            } else {
                text_str(&t, "0,");
                text_u32(&t, ses->dns_err);
            }
            modem_info(m, &t);
            return;
        case SIM7080G_EV_NTP:
            text_str(&t, "+CNTP: ");
            text_u32(&t, arg);
            modem_info(m, &t);
            if (arg == SIM7080G_CNTP_OK && ses->ntp_tz_set) {
                m->set.timezone = ses->ntp_tz;
            }
            return;
        case SIM7080G_EV_FTPGET:
            if (ses->ftp_state == SIM7080G_FTP_IDLE) {
                return;
            }
            text_str(&t, "+FTPGET: 1,");
            text_u32(&t, arg);
            modem_info(m, &t);
            if (arg == SIM7080G_FTP_DATA) {
                ses->ftp_state = SIM7080G_FTP_ACTIVE;
                if (ses->ftp_off == ses->ftp_len) {
                    modem_event(m, 0, SIM7080G_EV_FTPGET, SIM7080G_FTP_DONE);
                }
            } else if (arg != SIM7080G_FTP_DONE) {
                ses->ftp_state = SIM7080G_FTP_IDLE;
            }
            return;
        default:
            sim7080g_socket_event(s, kind, arg);
            return;
    }
}

static void bearer_lost(struct modem *m) {
    modem_event(m, 0, SIM7080G_EV_APP_PDP, 0);
    sim7080g_drop_sockets(sim7080g_of(m), 0);
}

// Manual 3.2.19 example: the boot URCs after CFUN=1,1, in that order. The
// Zephyr driver needs RDY and +CPIN: READY; +CFUN: 1 must come before
// +CPIN, because any unmatched line during its identity queries overwrites
// a stored field.
static void booted(struct modem *m) {
    modem_out_str(m, "\r\nRDY\r\n");
    modem_out_str(m, "\r\n+CFUN: 1\r\n");
    modem_line(m, modem_cpin_line(modem_sim_cpin(m)));
    modem_out_str(m, "\r\nSMS Ready\r\n");
}

static void session(struct modem *m) {
    struct sim7080g_session *ses = &sim7080g_of(m)->ses;

    memset(ses, 0, sizeof(*ses));
    ses->cnmp = SIM7080G_CNMP_LTE;
    ses->cmnb = SIM7080G_CMNB_BOTH;
    ses->sslversion = 3;
}

static const struct modem_part sim7080g_part = {
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
    { "+CREBOOT", cmd_creboot },
    { "+CLTS", modem_cmd_accept },
    { "+CBATCHK", modem_cmd_accept },
    { "+SGPIO", modem_cmd_accept },
    { "+CSCLK", modem_cmd_accept },
    { "+CPSI", cmd_cpsi },
    { "+CNMP", cmd_cnmp },
    { "+CMNB", cmd_cmnb },
    { "+CBANDCFG", modem_cmd_accept },
    { "+CGDCONT", cmd_cgdcont },
    { "+CGNAPN", cmd_cgnapn },
    { "+CNCFG", cmd_cncfg },
    { "+CNACT", cmd_cnact },
    { "+CGNSPWR", cmd_cgnspwr },
    { "+CGNSCOLD", modem_cmd_accept },
    { "+CGNSWARM", modem_cmd_accept },
    { "+CGNSHOT", modem_cmd_accept },
    { "+CGNSINF", cmd_cgnsinf },
    { "+CGNSCPY", modem_cmd_unsupported },
    { "+CGNSXTRA", modem_cmd_unsupported },
    { "+CLBS", cmd_clbs },
    { "+CUSD", modem_cmd_unsupported },
    { "+FTPCID", modem_cmd_accept },
    { "+FTPSERV", modem_cmd_accept },
    { "+FTPUN", modem_cmd_accept },
    { "+FTPPW", modem_cmd_accept },
    { "+FTPGETNAME", cmd_ftpgetname },
    { "+FTPGETPATH", cmd_ftpgetpath },
    { "+FTPGET", cmd_ftpget },
    { "+HTTPTOFS", modem_cmd_unsupported },
    { "+CNTPCID", modem_cmd_accept },
    { "+CNTP", cmd_cntp },
};

static const struct modem_commands own_commands = MODEM_COMMANDS(commands);

static const struct modem_commands *const tables[] = {
    &own_commands,
    &sim7080g_socket_commands,
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
    &sim7080g_part,
};

_Static_assert(sizeof(properties) / sizeof(properties[0]) ==
        MODEM_PROPERTY_COUNT + NET_PROPERTY_COUNT + GNSS_PROPERTY_COUNT + SMS_PROPERTY_COUNT,
    "the property table and the parts disagree");
_Static_assert(sizeof(regions) / sizeof(regions[0]) == NET_REGION_COUNT + SMS_REGION_COUNT,
    "the region table and the parts disagree");

static const struct mclw_binding_desc bindings[] = MODEM_BINDINGS(SIM7080G_DEFAULT_BAUD);

// Manual 2.2.32 rates; autobauding (0) and the rates above 921600 are not
// modelled.
static const uint32_t rates[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };

static const struct modem_config config = {
    .manufacturer = SIM7080G_MANUFACTURER,
    .model = SIM7080G_MODEL,
    .revision = SIM7080G_REVISION,
    .product = SIM7080G_PRODUCT,
    .imei = SIM7080G_IMEI,
    .imsi = SIM7080G_IMSI,
    .iccid = SIM7080G_ICCID,
    .operator_name = SIM7080G_OPERATOR,
    .rates = rates,
    .rate_count = sizeof(rates) / sizeof(rates[0]),
    .ipr_test = "+IPR: (),(9600,19200,38400,57600,115200,230400,460800,921600)",
    .cfun_levels = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7),
    .cfun_reboot = 1u << 6,
    .cfun_test = "+CFUN: (0-1,4-7),(0-1)",
    .pwrkey_on_us = SIM7080G_PWRKEY_ON_US,
    .pwrkey_off_us = SIM7080G_PWRKEY_OFF_US,
    .boot_ms = SIM7080G_DEFAULT_BOOT_MS,
    .power_down = "NORMAL POWER DOWN",
    .command_room = SIM7080G_COMMAND_ROOM,
    .urc_room = SIM7080G_URC_ROOM,
    .tables = tables,
    .table_count = sizeof(tables) / sizeof(tables[0]),
    .parts = parts,
    .part_count = sizeof(parts) / sizeof(parts[0]),
    .booted = booted,
    .event = event,
    .bearer_lost = bearer_lost,
    .access_technology = access_technology,
};

static int sim7080g_init(struct mclw_instance *inst, const struct mclw_binding *bindings_in) {
    struct sim7080g *s = inst->state;

    s->m.net = &s->net;
    s->m.sms = &s->sms;
    s->m.gnss = &s->gnss;
    net_init(&s->net, inst, s->sockets, SIM7080G_SOCKETS, sim7080g_net_notify, s);
    return modem_init(&s->m, inst, bindings_in, s->events, SIM7080G_EVENTS);
}

static const struct mclw_model_ops ops = {
    .init = sim7080g_init,
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

const struct mclw_model mclw_model_sim7080g = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "sim7080g",
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
    .state_size = sizeof(struct sim7080g),
    .ops = &ops,
};
