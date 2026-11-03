// SPDX-License-Identifier: Apache-2.0 OR MIT
// What 3GPP TS 27.007 and ITU-T V.250 define and every model answers the
// same way: identity, error reporting, the UART rate, functionality, the
// SIM, signal and registration, attach, the operator, the battery and the
// clock; and the registration and SIM URCs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "modem.h"
#include "text.h"

static const char *const cpin_lines[] = {
    [MODEM_CPIN_READY] = "+CPIN: READY",
    [MODEM_CPIN_NOT_READY] = "+CPIN: NOT READY",
    [MODEM_CPIN_NOT_INSERTED] = "+CPIN: NOT INSERTED",
    [MODEM_CPIN_SIM_PIN] = "+CPIN: SIM PIN",
};

const char *modem_cpin_line(uint8_t cpin) {
    return cpin < sizeof(cpin_lines) / sizeof(cpin_lines[0]) ? cpin_lines[cpin] : NULL;
}

bool modem_sim_ready(const struct modem *m) {
    return m->set.sim == 0 || (m->set.sim == 2 && m->ses.pin_entered);
}

uint8_t modem_sim_cpin(const struct modem *m) {
    static const uint8_t cpin[] = { MODEM_CPIN_READY, MODEM_CPIN_NOT_INSERTED, MODEM_CPIN_SIM_PIN };

    return cpin[m->set.sim];
}

uint8_t modem_stat(const struct modem *m) {
    if (m->power != MODEM_ON || !modem_sim_ready(m) || m->ses.cfun != 1) {
        return MODEM_REG_NONE;
    }
    return m->set.registration;
}

// 27.007 7.2 and 10.1.19: +CREG is the circuit domain, +CGREG and +CEREG
// the packet domain, which a PS detach (AT+CGATT=0) leaves.
uint8_t modem_ps_stat(const struct modem *m) {
    return m->ses.detached ? MODEM_REG_NONE : modem_stat(m);
}

bool modem_registered(const struct modem *m) {
    uint8_t stat = modem_stat(m);

    return stat == MODEM_REG_HOME || stat == MODEM_REG_ROAMING;
}

bool modem_attached(const struct modem *m) {
    return modem_registered(m) && !m->ses.detached;
}

void modem_network_changed(struct modem *m) {
    struct modem_session *ses = &m->ses;

    if (m->power != MODEM_ON) {
        return;
    }
    uint8_t stat = modem_stat(m);
    uint8_t ps = modem_ps_stat(m);
    uint8_t which = 0;
    if (stat != ses->reported_stat) {
        ses->reported_stat = stat;
        if (ses->creg_n != 0) {
            which |= MODEM_REG_URC_CREG;
        }
    }
    if (ps != ses->reported_ps_stat) {
        ses->reported_ps_stat = ps;
        if (ses->cgreg_n != 0) {
            which |= MODEM_REG_URC_CGREG;
        }
        if (ses->cereg_n != 0) {
            which |= MODEM_REG_URC_CEREG;
        }
    }
    if (which != 0) {
        modem_event(m, 0, MODEM_EV_REG, (uint8_t)(which | stat));
    }
    if (!modem_attached(m) && m->net != NULL && m->net->ses.bearer) {
        m->net->ses.bearer = false;
        m->config->bearer_lost(m);
    }
}

void modem_standard_event(struct modem *m, uint8_t kind, uint8_t arg) {
    static const struct {
        uint8_t bit;
        bool ps;
        const char *prefix;
    } urcs[] = {
        { MODEM_REG_URC_CREG, false, "+CREG: " },
        { MODEM_REG_URC_CGREG, true, "+CGREG: " },
        { MODEM_REG_URC_CEREG, true, "+CEREG: " },
    };
    struct text t;

    if (kind == MODEM_EV_CPIN) {
        if (modem_cpin_line(arg) != NULL) {
            modem_info_str(m, modem_cpin_line(arg));
        }
        return;
    }
    for (size_t i = 0; i < sizeof(urcs) / sizeof(urcs[0]); i++) {
        if ((arg & urcs[i].bit) != 0) {
            text_init(&t);
            text_str(&t, urcs[i].prefix);
            text_u32(&t, urcs[i].ps ? modem_ps_stat(m) : modem_stat(m));
            modem_info(m, &t);
        }
    }
}

void modem_identity(struct modem *m, enum modem_mode mode, const char *value) {
    if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (mode == MODEM_EXEC) {
        modem_answer(m, value);
    } else {
        modem_error(m);
    }
}

bool modem_require_sim(struct modem *m) {
    if (modem_sim_ready(m)) {
        return true;
    }
    modem_cme(m, m->set.sim == 1 ? MODEM_CME_SIM_NOT_INSERTED : MODEM_CME_SIM_PIN_REQUIRED);
    return false;
}

void modem_cmd_accept(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)mode;
    (void)a;
    modem_ok(m);
}

void modem_cmd_unsupported(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)mode;
    (void)a;
    modem_cme(m, MODEM_CME_NOT_SUPPORTED);
}

void modem_small_setting(struct modem *m, enum modem_mode mode, struct modem_args *a, const char *prefix,
    uint8_t *value, const uint8_t *allowed, size_t allowed_count) {
    int32_t v;

    if (mode == MODEM_READ) {
        modem_answer_u32(m, prefix, *value);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, 255, &v)) {
        for (size_t i = 0; i < allowed_count; i++) {
            if (allowed[i] == v) {
                *value = (uint8_t)v;
                modem_ok(m);
                return;
            }
        }
    }
    modem_error(m);
}

static void cmd_cgmi(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    modem_identity(m, mode, m->config->manufacturer);
}

static void cmd_cgmm(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    modem_identity(m, mode, m->config->model);
}

static void cmd_cgmr(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    modem_identity(m, mode, m->config->revision);
}

static void cmd_cgsn(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    modem_identity(m, mode, m->config->imei);
}

static void cmd_cimi(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode != MODEM_EXEC || modem_require_sim(m)) {
        modem_identity(m, mode, m->config->imsi);
    }
}

static void cmd_ccid(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode == MODEM_EXEC && m->set.sim == 1) {
        modem_cme(m, MODEM_CME_SIM_NOT_INSERTED);
        return;
    }
    modem_identity(m, mode, m->config->iccid);
}

static void cmd_cmee(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t n = 0;

    if (mode == MODEM_READ) {
        modem_answer_u32(m, "+CMEE: ", m->ses.cmee);
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CMEE: (0-2)");
    } else if (mode == MODEM_EXEC || (a->count == 1 && a->v[0].str[0] == '\0') || modem_arg_int(a, 0, 0, 2, &n)) {
        m->ses.cmee = (uint8_t)n;
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

static bool valid_rate(const struct modem *m, int32_t rate) {
    for (size_t i = 0; i < m->config->rate_count; i++) {
        if ((int32_t)m->config->rates[i] == rate) {
            return true;
        }
    }
    return false;
}

static void cmd_ipr(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t rate;

    if (mode == MODEM_READ) {
        modem_answer_u32(m, "+IPR: ", m->set.baud);
    } else if (mode == MODEM_TEST) {
        modem_answer(m, m->config->ipr_test);
    } else if (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, INT32_MAX, &rate) && valid_rate(m, rate)) {
        modem_ok(m);
        if ((uint32_t)rate != m->set.baud) {
            m->ses.pending_baud = (uint32_t)rate;
        }
    } else {
        modem_error(m);
    }
}

static void cmd_cfun(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    const struct modem_config *c = m->config;
    int32_t fun;
    int32_t rst = 0;

    if (mode == MODEM_READ) {
        modem_answer_u32(m, "+CFUN: ", m->ses.cfun);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_answer(m, c->cfun_test);
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 7, &fun) || (c->cfun_levels & (1u << fun)) == 0 ||
        (a->count > 1 && !modem_arg_int(a, 1, 0, 1, &rst))) {
        modem_error(m);
        return;
    }
    modem_ok(m);
    if (rst == 1 || (c->cfun_reboot & (1u << fun)) != 0) {
        modem_power_on(m);
        return;
    }
    uint8_t before = m->ses.cfun;
    m->ses.cfun = (uint8_t)fun;
    if (modem_sim_ready(m) && (before == 1) != (fun == 1)) {
        modem_event(m, 0, MODEM_EV_CPIN, fun == 1 ? MODEM_CPIN_READY : MODEM_CPIN_NOT_READY);
    }
    modem_network_changed(m);
}

static void cmd_cpin(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (m->set.sim == 1) {
        modem_cme(m, MODEM_CME_SIM_NOT_INSERTED);
    } else if (mode == MODEM_READ) {
        modem_answer(m, modem_sim_ready(m) ? "+CPIN: READY" : "+CPIN: SIM PIN");
    } else if (mode == MODEM_WRITE && !modem_sim_ready(m) && a->count >= 1 && a->v[0].str[0] != '\0') {
        m->ses.pin_entered = true;
        modem_ok(m);
        modem_event(m, 0, MODEM_EV_CPIN, MODEM_CPIN_READY);
        modem_network_changed(m);
    } else {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
    }
}

static void cmd_csq(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;

    (void)a;
    if (mode == MODEM_TEST) {
        modem_answer(m, "+CSQ: (0-31,99),(0-7,99)");
        return;
    }
    if (mode != MODEM_EXEC) {
        modem_error(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+CSQ: ");
    text_u32(&t, m->ses.cfun == 1 ? m->set.rssi : MODEM_RSSI_UNKNOWN);
    text_str(&t, ",99");
    modem_answer_text(m, &t);
}

static void registration(struct modem *m, enum modem_mode mode, struct modem_args *a, const char *prefix,
    uint8_t *n, int32_t n_max, bool ps) {
    struct text t;
    int32_t v;

    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, prefix);
        text_u32(&t, *n);
        text_char(&t, ',');
        text_u32(&t, ps ? modem_ps_stat(m) : modem_stat(m));
        modem_answer_text(m, &t);
    } else if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (mode == MODEM_EXEC || modem_arg_int(a, 0, 0, n_max, &v)) {
        *n = mode == MODEM_EXEC ? 0 : (uint8_t)v;
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

// Location reporting (<n>=2 and 4) is accepted but reported like <n>=1.
static void cmd_creg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    registration(m, mode, a, "+CREG: ", &m->ses.creg_n, 2, false);
}

static void cmd_cgreg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    registration(m, mode, a, "+CGREG: ", &m->ses.cgreg_n, 2, true);
}

static void cmd_cereg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    registration(m, mode, a, "+CEREG: ", &m->ses.cereg_n, 4, true);
}

static void cmd_cgatt(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t v;

    if (mode == MODEM_READ) {
        modem_answer(m, modem_attached(m) ? "+CGATT: 1" : "+CGATT: 0");
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CGATT: (0,1)");
    } else if (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, 1, &v)) {
        if (v == 1 && !modem_registered(m)) {
            modem_cme(m, MODEM_CME_NO_NETWORK);
            return;
        }
        m->ses.detached = v == 0;
        modem_ok(m);
        modem_network_changed(m);
    } else {
        modem_error(m);
    }
}

static void cmd_cops(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;

    (void)a;
    if (mode != MODEM_READ) {
        modem_ok(m);
        return;
    }
    if (!modem_registered(m)) {
        modem_answer(m, "+COPS: 0");
        return;
    }
    text_init(&t);
    text_str(&t, "+COPS: 0,0,\"");
    text_str(&t, m->config->operator_name);
    text_str(&t, "\",");
    text_u32(&t, m->config->access_technology(m));
    modem_answer_text(m, &t);
}

static void cmd_cbc(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;

    (void)a;
    if (mode == MODEM_TEST) {
        modem_answer(m, "+CBC: (0-2),(1-100),(voltage)");
        return;
    }
    if (mode != MODEM_EXEC) {
        modem_error(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+CBC: 0,");
    text_u32(&t, m->set.battery_percent);
    text_char(&t, ',');
    text_u32(&t, m->set.battery_mv);
    modem_answer_text(m, &t);
}

static bool leap(int32_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int32_t days_in_month(int32_t year, int32_t month) {
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month < 1 || month > 12) {
        return 31;
    }
    return days[month - 1] + (month == 2 && leap(year) ? 1 : 0);
}

// A UTC date both a 4-digit year and the 2-digit one of +CCLK can carry.
bool modem_time_valid(const int32_t time[6]) {
    return time[0] >= MODEM_YEAR_MIN && time[0] <= MODEM_YEAR_MAX && time[1] >= 1 && time[1] <= 12 &&
        time[2] >= 1 && time[2] <= days_in_month(time[0], time[1]) && time[3] >= 0 && time[3] < 24 &&
        time[4] >= 0 && time[4] < 60 && time[5] >= 0 && time[5] < 60;
}

static int32_t floor_div(int32_t a, int32_t b) {
    int32_t q = a / b;

    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

// Shifts a date and time by at most a day either way (a time zone).
static void utc_to_local(const int32_t utc[6], int32_t minutes, int32_t out[6]) {
    memcpy(out, utc, 6 * sizeof(out[0]));
    int32_t mins = out[4] + minutes;
    int32_t h = out[3] + floor_div(mins, 60);
    out[4] = mins - floor_div(mins, 60) * 60;
    int32_t day_shift = floor_div(h, 24);
    out[3] = h - day_shift * 24;
    if (day_shift > 0) {
        out[2]++;
        if (out[2] > days_in_month(out[0], out[1])) {
            out[2] = 1;
            if (++out[1] > 12) {
                out[1] = 1;
                out[0]++;
            }
        }
    } else if (day_shift < 0) {
        out[2]--;
        if (out[2] < 1) {
            if (--out[1] < 1) {
                out[1] = 12;
                out[0]--;
            }
            out[2] = days_in_month(out[0], out[1]);
        }
    }
}

static bool parse_digits(const char **p, size_t count, int32_t *out) {
    int32_t v = 0;

    for (size_t i = 0; i < count; i++) {
        char c = (*p)[i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    *p += count;
    *out = v;
    return true;
}

static bool expect(const char **p, char c) {
    if (**p != c) {
        return false;
    }
    (*p)++;
    return true;
}

// "yy/MM/dd,hh:mm:ss[+-zz]", local time.
static bool parse_cclk(const char *p, int32_t local[6], int32_t *tz) {
    static const char seps[6] = { '/', '/', ',', ':', ':', '\0' };

    for (size_t i = 0; i < 6; i++) {
        if (!parse_digits(&p, 2, &local[i]) || (seps[i] != '\0' && !expect(&p, seps[i]))) {
            return false;
        }
    }
    local[0] += 2000;
    *tz = 0;
    if (*p == '+' || *p == '-') {
        bool negative = *p == '-';
        p++;
        if (!parse_digits(&p, 2, tz)) {
            return false;
        }
        *tz = negative ? -*tz : *tz;
    }
    return *p == '\0' && local[1] >= 1 && local[1] <= 12 && local[2] >= 1 &&
        local[2] <= days_in_month(local[0], local[1]) && local[3] < 24 && local[4] < 60 && local[5] < 60 &&
        *tz >= MODEM_TIMEZONE_MIN && *tz <= MODEM_TIMEZONE_MAX;
}

// 27.007 8.15: local time, with the zone in quarter hours.
static void cmd_cclk(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;
    int32_t local[6];
    int32_t tz;

    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode == MODEM_WRITE) {
        if (a->count != 1 || !a->v[0].quoted || !parse_cclk(a->v[0].str, local, &tz)) {
            modem_error(m);
            return;
        }
        utc_to_local(local, -tz * 15, m->set.time);
        m->set.timezone = (int8_t)tz;
        modem_ok(m);
        return;
    }
    if (mode != MODEM_READ) {
        modem_error(m);
        return;
    }
    utc_to_local(m->set.time, m->set.timezone * 15, local);
    text_init(&t);
    text_str(&t, "+CCLK: \"");
    text_date_time(&t, local);
    text_char(&t, m->set.timezone < 0 ? '-' : '+');
    text_d2(&t, (uint32_t)(m->set.timezone < 0 ? -m->set.timezone : m->set.timezone));
    text_char(&t, '"');
    modem_answer_text(m, &t);
}

static const struct modem_command standard[] = {
    { "+CGMI", cmd_cgmi },
    { "+CGMM", cmd_cgmm },
    { "+CGMR", cmd_cgmr },
    { "+CGSN", cmd_cgsn },
    { "+GSN", cmd_cgsn },
    { "+CIMI", cmd_cimi },
    { "+CCID", cmd_ccid },
    { "+CMEE", cmd_cmee },
    { "+IPR", cmd_ipr },
    { "+CFUN", cmd_cfun },
    { "+CPIN", cmd_cpin },
    { "+CSQ", cmd_csq },
    { "+CREG", cmd_creg },
    { "+CGREG", cmd_cgreg },
    { "+CEREG", cmd_cereg },
    { "+CGATT", cmd_cgatt },
    { "+COPS", cmd_cops },
    { "+CBC", cmd_cbc },
    { "+CCLK", cmd_cclk },
};

const struct modem_commands modem_standard_commands = MODEM_COMMANDS(standard);
