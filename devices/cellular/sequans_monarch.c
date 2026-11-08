// SPDX-License-Identifier: Apache-2.0 OR MIT
// The Sequans Monarch model: its descriptors and identity, boot and
// functionality, PDP contexts, time zone and power-saving settings, the
// thermistor and signal quality, and shutdown.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "modem.h"
#include "net.h"
#include "sequans_monarch.h"
#include "sms.h"
#include "text.h"

enum {
    PROP_TEMPERATURE,
    PROP_COUNT,
};

struct sequans_monarch *sequans_monarch_of(struct modem *m) {
    return (struct sequans_monarch *)m;
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

// LR8.2 CFUN, CPIN, CIMI, SQNCCID: the SIM is reachable at CFUN 1 and 4
// (and 5, RF off for manufacturing), not at the minimum functionality the
// part boots in.
static bool sim_powered(const struct modem *m) {
    return m->ses.cfun != 0;
}

static void cmd_error(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)mode;
    (void)a;
    modem_error(m);
}

// LR8.2 CFUN: only 0, 1, 4 and 5 can be set. CFUN=0 answers once detached
// and +CEREG: 0 follows its OK; CFUN=1 answers at once and registration
// follows. <rst> 1 resets first: OK, +SHUTDOWN, and the boot comes back at
// <fun>.
static void cmd_cfun(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    int32_t fun;
    int32_t rst = 0;

    if (mode == MODEM_READ) {
        modem_answer_u32(m, "+CFUN: ", m->ses.cfun);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_answer(m, m->config->cfun_test);
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 7, &fun) || (m->config->cfun_levels & (1u << fun)) == 0 ||
        (a->count > 1 && !modem_arg_int(a, 1, 0, 1, &rst))) {
        modem_error(m);
        return;
    }
    modem_ok(m);
    if (rst == 1) {
        s->nv.boot_cfun = (uint8_t)fun;
        modem_line(m, "+SHUTDOWN");
        modem_power_on(m);
        return;
    }
    m->ses.cfun = (uint8_t)fun;
    modem_network_changed(m);
}

// LR5 8.3 and LR8.2: a hardware reset. The manuals list no OK, only
// +SHUTDOWN and then the boot's +SYSSTART, at the CFUN the part boots in.
static void cmd_reset(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (mode == MODEM_EXEC) {
        modem_line(m, "+SHUTDOWN");
        modem_power_on(m);
    } else {
        modem_error(m);
    }
}

static void cmd_cpin(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    if (mode != MODEM_TEST && !sim_powered(m)) {
        modem_cme(m, MODEM_CME_SIM_NOT_INSERTED);
        return;
    }
    standard_handler("+CPIN")(m, mode, a);
}

static void cmd_cimi(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    if (mode == MODEM_EXEC && !sim_powered(m)) {
        modem_cme(m, MODEM_CME_SIM_NOT_INSERTED);
        return;
    }
    standard_handler("+CIMI")(m, mode, a);
}

// LR8.2 CEREG: <n> 0..5; 2 to 5 report like 1.
static void cmd_cereg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t n;

    if (mode == MODEM_WRITE && modem_arg_int(a, 0, 5, 5, &n)) {
        m->ses.cereg_n = (uint8_t)n;
        modem_ok(m);
        return;
    }
    standard_handler("+CEREG")(m, mode, a);
}

// LR8.2 CGSN: <snt> 0 (or none) is the bare IMEI, 1 the IMEI with the
// command's prefix.
static void cmd_cgsn(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;
    int32_t snt;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CGSN: (0-1)");
    } else if (mode == MODEM_EXEC || (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, 0, &snt))) {
        modem_answer(m, m->config->imei);
    } else if (mode == MODEM_WRITE && modem_arg_int(a, 0, 1, 1, &snt)) {
        text_init(&t);
        text_str(&t, "+CGSN: \"");
        text_str(&t, m->config->imei);
        text_char(&t, '"');
        modem_answer_text(m, &t);
    } else {
        modem_error(m);
    }
}

// LR5 10.3 and LR8.2: both forms read the ICCID, and below CFUN 1 or 4 the
// read "silently fails": OK alone. The euiccid is empty.
static void cmd_sqnccid(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;

    (void)a;
    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode == MODEM_WRITE) {
        modem_error(m);
        return;
    }
    if (m->set.sim == 1) {
        modem_cme(m, MODEM_CME_SIM_NOT_INSERTED);
        return;
    }
    if (!sim_powered(m)) {
        modem_ok(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+SQNCCID: \"");
    text_str(&t, m->config->iccid);
    text_str(&t, "\",\"\"");
    modem_answer_text(m, &t);
}

static void cmd_ctzr(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    static const uint8_t allowed[] = { 0, 1, 2 };

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CTZR: (0-2)");
        return;
    }
    modem_small_setting(m, mode, a, "+CTZR: ", &sequans_monarch_of(m)->nv.ctzr, allowed, sizeof(allowed));
}

static void cmd_ctzu(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    static const uint8_t allowed[] = { 0, 1 };

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CTZU: (0,1)");
        return;
    }
    modem_small_setting(m, mode, a, "+CTZU: ", &sequans_monarch_of(m)->nv.ctzu, allowed, sizeof(allowed));
}

// LR5 16.8: stored for the next boot; the UART never sleeps.
static void cmd_sqnipscfg(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch_nv *nv = &sequans_monarch_of(m)->nv;
    struct text t;
    int32_t ips;
    int32_t timeout = nv->ips_timeout;

    if (mode == MODEM_READ) {
        text_init(&t);
        text_str(&t, "+SQNIPSCFG: ");
        text_u32(&t, nv->ips_mode);
        text_char(&t, ',');
        text_u32(&t, nv->ips_timeout);
        modem_answer_text(m, &t);
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+SQNIPSCFG: (0-2),(100-10000)");
    } else if (mode == MODEM_WRITE && modem_arg_int(a, 0, 0, 2, &ips) &&
        (a->count < 2 || modem_arg_int(a, 1, 100, 10000, &timeout))) {
        nv->ips_mode = (uint8_t)ips;
        nv->ips_timeout = (uint16_t)timeout;
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

// LR5 8.5 shows `+SMDTH:<temperature>`; TinyGSM waits for `+SMDTH: ` with
// the space 27.007 puts after every other prefix.
static void cmd_smdth(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;

    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode == MODEM_READ || (mode == MODEM_WRITE && (a->count > 1 || a->v[0].str[0] != '\0'))) {
        modem_error(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+SMDTH: ");
    text_i32(&t, sequans_monarch_of(m)->set.temperature);
    modem_answer_text(m, &t);
}

// LR8.2 CESQ: only <rsrq> and <rsrp> mean anything, and only at CFUN 1.
// RSRP is 10 dB below the rssi property's -113 + 2n dBm, RSRQ a fixed
// -10 dB, each coded as 27.007 tables them.
static void cmd_cesq(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct text t;

    (void)a;
    if (mode == MODEM_TEST) {
        modem_answer(m, "+CESQ: (99),(99),(255),(255),(0-34,255),(0-97,255)");
        return;
    }
    if (mode != MODEM_EXEC) {
        modem_error(m);
        return;
    }
    text_init(&t);
    text_str(&t, "+CESQ: 99,99,255,255,");
    if (m->ses.cfun != 1 || m->set.rssi == MODEM_RSSI_UNKNOWN) {
        text_str(&t, "255,255");
    } else {
        int32_t rsrp = -113 + 2 * (int32_t)m->set.rssi - 10;
        text_str(&t, "20,");
        text_u32(&t, (uint32_t)(rsrp + 141));
    }
    modem_answer_text(m, &t);
}

// LR5 8.7: OK, then +SQNSSHDN once the power can be cut; the part then
// answers nothing until RESETN.
static void cmd_sqnsshdn(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode == MODEM_TEST) {
        modem_ok(m);
    } else if (mode == MODEM_EXEC) {
        modem_ok(m);
        modem_power_off(m, true);
    } else {
        modem_error(m);
    }
}

static const char *const pdp_types[] = {
    [SEQUANS_MONARCH_PDP_IP] = "IP",
    [SEQUANS_MONARCH_PDP_IPV6] = "IPV6",
    [SEQUANS_MONARCH_PDP_IPV4V6] = "IPV4V6",
};

static struct sequans_monarch_context *context_at(struct sequans_monarch *s, int32_t cid) {
    return &s->nv.contexts[cid - 1];
}

static bool defined(const struct sequans_monarch *s, int32_t cid) {
    return cid >= 1 && cid <= SEQUANS_MONARCH_CONTEXTS && s->nv.contexts[cid - 1].type != SEQUANS_MONARCH_PDP_UNDEFINED;
}

// The context bit of the PPP call, 0 without one or for a cid past the
// contexts.
static uint8_t ppp_bit(const struct sequans_monarch *s) {
    return s->ses.ppp_cid != 0 && s->ses.ppp_cid <= SEQUANS_MONARCH_CONTEXTS ? (uint8_t)(1u << s->ses.ppp_cid) : 0;
}

// The contexts CGACT or the PPP call hold up.
static uint8_t up_mask(const struct sequans_monarch *s) {
    return (uint8_t)(s->ses.active | ppp_bit(s));
}

bool sequans_monarch_context_up(const struct sequans_monarch *s, uint8_t cid) {
    return defined(s, cid) && (up_mask(s) & (1u << cid)) != 0 && s->net.ses.bearer;
}

static void context_defaults(struct sequans_monarch *s) {
    memset(s->nv.contexts, 0, sizeof(s->nv.contexts));
    context_at(s, 1)->type = SEQUANS_MONARCH_PDP_IPV4V6;
}

static void cgdcont_list(struct modem *m) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    struct text t;
    bool any = false;

    for (int32_t cid = 1; cid <= SEQUANS_MONARCH_CONTEXTS; cid++) {
        if (!defined(s, cid)) {
            continue;
        }
        if (!any) {
            modem_out_str(m, "\r\n");
            any = true;
        }
        text_init(&t);
        text_str(&t, "+CGDCONT: ");
        text_u32(&t, (uint32_t)cid);
        text_str(&t, ",\"");
        text_str(&t, pdp_types[context_at(s, cid)->type]);
        text_str(&t, "\",\"");
        text_str(&t, context_at(s, cid)->apn);
        text_str(&t, "\",\"\",0,0");
        modem_info_line(m, &t);
    }
    modem_ok(m);
}

// LR8.2 CGDCONT: a context cannot change while the part is attached;
// AT+CGDCONT=<cid> undefines one. Only the EPS types are supported.
static void cmd_cgdcont(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    int32_t cid;
    uint8_t type = SEQUANS_MONARCH_PDP_UNDEFINED;

    if (mode == MODEM_READ) {
        cgdcont_list(m);
        return;
    }
    if (mode == MODEM_TEST) {
        modem_out_str(m, "\r\n");
        for (size_t i = SEQUANS_MONARCH_PDP_IP; i <= SEQUANS_MONARCH_PDP_IPV4V6; i++) {
            struct text t;
            text_init(&t);
            text_str(&t, "+CGDCONT: (1-6),\"");
            text_str(&t, pdp_types[i]);
            text_str(&t, "\",,,(0),(0)");
            modem_info_line(m, &t);
        }
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 1, SEQUANS_MONARCH_CONTEXTS, &cid)) {
        modem_error(m);
        return;
    }
    if (a->count > 1) {
        for (size_t i = SEQUANS_MONARCH_PDP_IP; i <= SEQUANS_MONARCH_PDP_IPV4V6; i++) {
            if (text_equal_nocase(a->v[1].str, pdp_types[i])) {
                type = (uint8_t)i;
            }
        }
        if (type == SEQUANS_MONARCH_PDP_UNDEFINED) {
            modem_error(m);
            return;
        }
    }
    if (modem_attached(m)) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    struct sequans_monarch_context *ctx = context_at(s, cid);
    memset(ctx, 0, sizeof(*ctx));
    ctx->type = type;
    if (a->count > 2) {
        size_t n = text_strnlen(a->v[2].str, SEQUANS_MONARCH_APN_MAX - 1);
        memcpy(ctx->apn, a->v[2].str, n);
    }
    s->ses.active = (uint8_t)(s->ses.active & ~(1u << cid));
    modem_ok(m);
}

// LR8.2 CGAUTH: the credentials are checked for form and not kept.
static void cmd_cgauth(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    int32_t cid;
    int32_t prot;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CGAUTH: (1-6),(0-2),,");
    } else if (mode == MODEM_READ) {
        modem_ok(m);
    } else if (mode == MODEM_WRITE && modem_arg_int(a, 0, 1, SEQUANS_MONARCH_CONTEXTS, &cid) &&
        (a->count < 2 || modem_arg_int(a, 1, 0, 2, &prot))) {
        modem_ok(m);
    } else {
        modem_error(m);
    }
}

static void bearer_lost(struct modem *m) {
    struct sequans_monarch *s = sequans_monarch_of(m);

    s->ses.active = 0;
    s->ses.ppp_cid = 0;
    sequans_monarch_drop_sockets(s);
    modem_link_bearer_lost(m);
}

// The last context takes the bearer with it.
static void contexts_down(struct sequans_monarch *s) {
    s->ses.active = 0;
    s->ses.ppp_cid = 0;
    if (s->net.ses.bearer) {
        s->net.ses.bearer = false;
        bearer_lost(&s->m);
    }
}

// The PPP call holds the context it dialled: CGACT? shows it up while the
// call is, and the call ending leaves the bearer to the contexts CGACT
// raised. With none left the sockets go with it.
static void link_bearer(struct modem *m, bool up) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    bool was = s->net.ses.bearer;

    s->ses.ppp_cid = up ? s->link.ppp_cid : 0;
    s->net.ses.bearer = up || s->ses.active != 0;
    if (was && !s->net.ses.bearer) {
        sequans_monarch_drop_sockets(s);
    }
}

// The <cid>s a CGACT or CGPADDR write names, or every defined one; false
// when one is out of range or undefined.
static bool cid_mask(const struct sequans_monarch *s, const struct modem_args *a, uint8_t first, uint8_t *mask) {
    int32_t cid;

    *mask = 0;
    if (a->count <= first) {
        for (cid = 1; cid <= SEQUANS_MONARCH_CONTEXTS; cid++) {
            if (defined(s, cid)) {
                *mask = (uint8_t)(*mask | (1u << cid));
            }
        }
        return true;
    }
    for (uint8_t i = first; i < a->count; i++) {
        if (!modem_arg_int(a, i, 1, SEQUANS_MONARCH_CONTEXTS, &cid) || !defined(s, cid)) {
            return false;
        }
        *mask = (uint8_t)(*mask | (1u << cid));
    }
    return true;
}

// LR8.2 CGACT: activating attaches first; for EPS the last PDN connection
// cannot be deactivated while attached; deactivating what is inactive
// changes nothing.
static void cmd_cgact(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    struct text t;
    int32_t state;
    uint8_t mask;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CGACT: (0,1)");
        return;
    }
    if (mode == MODEM_READ) {
        bool any = false;
        for (int32_t cid = 1; cid <= SEQUANS_MONARCH_CONTEXTS; cid++) {
            if (!defined(s, cid)) {
                continue;
            }
            if (!any) {
                modem_out_str(m, "\r\n");
                any = true;
            }
            text_init(&t);
            text_str(&t, "+CGACT: ");
            text_u32(&t, (uint32_t)cid);
            text_str(&t, sequans_monarch_context_up(s, (uint8_t)cid) ? ",1" : ",0");
            modem_info_line(m, &t);
        }
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 1, &state) || !cid_mask(s, a, 1, &mask)) {
        modem_error(m);
        return;
    }
    if (state == 1) {
        if (!modem_registered(m)) {
            modem_cme(m, MODEM_CME_NO_NETWORK);
            return;
        }
        m->ses.detached = false;
        s->ses.active = (uint8_t)(s->ses.active | mask);
        s->net.ses.bearer = true;
        modem_ok(m);
        return;
    }
    uint8_t up = up_mask(s);
    uint8_t left = (uint8_t)(up & ~mask);
    if (up != 0 && left == 0 && modem_attached(m)) {
        modem_cme(m, MODEM_CME_NOT_ALLOWED);
        return;
    }
    if (left == 0) {
        contexts_down(s);
    } else {
        s->ses.active = (uint8_t)(s->ses.active & ~mask);
        if ((mask & ppp_bit(s)) != 0) {
            // The call's context went down under it: PPP ends.
            s->ses.ppp_cid = 0;
            modem_link_bearer_lost(m);
        }
    }
    modem_ok(m);
}

// LR8.2 CGPADDR: a defined context's address, quoted, once it is active;
// only its <cid> while it is not.
static void cmd_cgpaddr(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    struct text t;
    uint8_t mask;
    bool any = false;

    if (mode == MODEM_TEST) {
        text_init(&t);
        text_str(&t, "+CGPADDR: (");
        for (int32_t cid = 1; cid <= SEQUANS_MONARCH_CONTEXTS; cid++) {
            if (defined(s, cid)) {
                if (any) {
                    text_char(&t, ',');
                }
                text_u32(&t, (uint32_t)cid);
                any = true;
            }
        }
        text_char(&t, ')');
        modem_answer_text(m, &t);
        return;
    }
    if (mode == MODEM_READ || !cid_mask(s, a, 0, &mask)) {
        modem_error(m);
        return;
    }
    for (uint8_t cid = 1; cid <= SEQUANS_MONARCH_CONTEXTS; cid++) {
        if ((mask & (1u << cid)) == 0) {
            continue;
        }
        if (!any) {
            modem_out_str(m, "\r\n");
            any = true;
        }
        text_init(&t);
        text_str(&t, "+CGPADDR: ");
        text_u32(&t, cid);
        if (sequans_monarch_context_up(s, cid)) {
            text_str(&t, ",\"");
            text_ip(&t, s->net.set.ip);
            text_char(&t, '"');
        }
        modem_info_line(m, &t);
    }
    modem_ok(m);
}

static uint8_t access_technology(const struct modem *m) {
    (void)m;
    return MODEM_ACT_CATM;
}

static void event(struct modem *m, uint8_t kind, uint8_t arg) {
    sequans_monarch_socket_event(sequans_monarch_of(m), kind, arg);
}

// LR5 8.6: +SYSSTART at every start, at the functionality the part boots
// in: 0, or what AT+CFUN=<fun>,1 asked for.
static void booted(struct modem *m) {
    modem_line(m, "+SYSSTART");
    sequans_monarch_of(m)->nv.boot_cfun = 0;
}

// Echo is off from boot: ATE's default is 0 in LR5 7.2 and LR8.2.
static void session(struct modem *m) {
    struct sequans_monarch *s = sequans_monarch_of(m);

    memset(&s->ses, 0, sizeof(s->ses));
    m->ses.cfun = s->nv.boot_cfun;
    m->ses.raw.echo = false;
}

static void nv_defaults(struct modem *m) {
    struct sequans_monarch *s = sequans_monarch_of(m);
    struct sequans_monarch_nv *nv = &s->nv;

    memset(nv, 0, sizeof(*nv));
    context_defaults(s);
    for (size_t i = 0; i < SEQUANS_MONARCH_SOCKETS; i++) {
        sequans_monarch_socket_defaults(&nv->sockets[i]);
    }
    nv->ips_mode = 1;
    nv->ips_timeout = 5000;
}

static void defaults(struct modem *m) {
    sequans_monarch_of(m)->set.temperature = SEQUANS_MONARCH_DEFAULT_TEMPERATURE;
    nv_defaults(m);
}

static int part_get(struct modem *m, size_t property, struct mclw_value *out) {
    if (property != PROP_TEMPERATURE) {
        return -EINVAL;
    }
    out->v[0] = sequans_monarch_of(m)->set.temperature;
    return 0;
}

static int part_set(struct modem *m, size_t property, const struct mclw_value *in) {
    if (property != PROP_TEMPERATURE) {
        return -EINVAL;
    }
    sequans_monarch_of(m)->set.temperature = (int16_t)in->v[0];
    return 0;
}

static const struct modem_part sequans_monarch_part = {
    .property_count = PROP_COUNT,
    .region_count = 0,
    .defaults = defaults,
    .clear = nv_defaults,
    .session = session,
    .get = part_get,
    .set = part_set,
    .region = NULL,
    .written = NULL,
};

// Neither manual has +CCID, +CGREG, +CBC or +CUSD; the last answers ERROR
// without counting as unknown, as TinyGSM sends it.
static const struct modem_command commands[] = {
    { "+CFUN", cmd_cfun },
    { "^RESET", cmd_reset },
    { "+CPIN", cmd_cpin },
    { "+CIMI", cmd_cimi },
    { "+CEREG", cmd_cereg },
    { "+CGSN", cmd_cgsn },
    { "+CCID", NULL },
    { "+CGREG", NULL },
    { "+CBC", NULL },
    { "+CUSD", cmd_error },
    { "+SQNCCID", cmd_sqnccid },
    { "+CTZR", cmd_ctzr },
    { "+CTZU", cmd_ctzu },
    { "+SQNIPSCFG", cmd_sqnipscfg },
    { "+SMDTH", cmd_smdth },
    { "+CESQ", cmd_cesq },
    { "+SQNSSHDN", cmd_sqnsshdn },
    { "+CGDCONT", cmd_cgdcont },
    { "+CGAUTH", cmd_cgauth },
    { "+CGACT", cmd_cgact },
    { "+CGPADDR", cmd_cgpaddr },
    { "+CMUX", modem_cmd_cmux },
    { "D", modem_cmd_dial_ppp },
};

static const struct modem_commands own_commands = MODEM_COMMANDS(commands);

static const struct modem_commands *const tables[] = {
    &own_commands,
    &sequans_monarch_socket_commands,
    &sms_commands,
    &modem_standard_commands,
};

static const struct mclw_property_desc properties[] = {
    MODEM_PROPERTIES,
    NET_PROPERTIES,
    SMS_PROPERTIES,
    PPP_PROPERTIES,
    MODEM_PROP("temperature", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "degC", SEQUANS_MONARCH_TEMPERATURE_MIN,
        SEQUANS_MONARCH_TEMPERATURE_MAX),
};

static const struct mclw_region_desc regions[] = {
    NET_REGIONS,
    SMS_REGIONS,
};

static const struct modem_part *const parts[] = {
    &modem_engine_part,
    &net_part,
    &sms_part,
    &ppp_part,
    &sequans_monarch_part,
};

_Static_assert(sizeof(properties) / sizeof(properties[0]) ==
        MODEM_PROPERTY_COUNT + NET_PROPERTY_COUNT + SMS_PROPERTY_COUNT + PPP_PROPERTY_COUNT + PROP_COUNT,
    "the property table and the parts disagree");
_Static_assert(sizeof(regions) / sizeof(regions[0]) == NET_REGION_COUNT + SMS_REGION_COUNT,
    "the region table and the parts disagree");

static const struct mclw_binding_desc bindings[] = MODEM_BINDINGS_RESET(SEQUANS_MONARCH_DEFAULT_BAUD);

// Neither manual lists the rates; autobauding (0) is not supported (LR8.2
// IPR).
static const uint32_t rates[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };

static const struct modem_config config = {
    .manufacturer = SEQUANS_MONARCH_MANUFACTURER,
    .model = SEQUANS_MONARCH_MODEL,
    .revision = SEQUANS_MONARCH_REVISION,
    .product = SEQUANS_MONARCH_PRODUCT,
    .imei = SEQUANS_MONARCH_IMEI,
    .imsi = SEQUANS_MONARCH_IMSI,
    .iccid = SEQUANS_MONARCH_ICCID,
    .operator_name = SEQUANS_MONARCH_OPERATOR,
    .rates = rates,
    .rate_count = sizeof(rates) / sizeof(rates[0]),
    .ipr_test = "+IPR: (),(9600,19200,38400,57600,115200,230400,460800,921600)",
    .cfun_levels = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5),
    .cfun_reboot = 0,
    .cfun_test = "+CFUN: (0,1,4,5),(0,1)",
    .pwrkey_on_us = 0,
    .pwrkey_off_us = 0,
    .boot_ms = SEQUANS_MONARCH_DEFAULT_BOOT_MS,
    .power_pin = MODEM_PIN_RESET,
    .reset_us = SEQUANS_MONARCH_RESET_US,
    .power_down = "+SQNSSHDN",
    .command_room = SEQUANS_MONARCH_COMMAND_ROOM,
    .urc_room = SEQUANS_MONARCH_URC_ROOM,
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

static int sequans_monarch_init(struct mclw_instance *inst, const struct mclw_binding *bindings_in) {
    struct sequans_monarch *s = inst->state;

    s->m.net = &s->net;
    s->m.sms = &s->sms;
    s->m.link = &s->link;
    net_init(&s->net, inst, s->sockets, SEQUANS_MONARCH_SOCKETS, sequans_monarch_net_notify, s);
    return modem_init(&s->m, inst, bindings_in, s->events, SEQUANS_MONARCH_EVENTS);
}

static const struct mclw_model_ops ops = {
    .init = sequans_monarch_init,
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

const struct mclw_model mclw_model_sequans_monarch = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "sequans_monarch",
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
    .state_size = sizeof(struct sequans_monarch),
    .ops = &ops,
};
