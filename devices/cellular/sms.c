// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/value.h"

#include "modem.h"
#include "sms.h"
#include "text.h"

// "\r\n+CMGL: 32,1,,191\r\n" and "\r\n" around a PDU, and the final OK. A
// list that would overflow the output FIFO ends early.
#define CMGL_ENTRY_OVERHEAD 32u

enum {
    REGION_INBOX,
    REGION_OUTBOX,
    REGION_END,
};

_Static_assert(REGION_END == SMS_REGION_COUNT, "SMS_REGIONS and the sms part disagree");

static bool sms_allowed(struct modem *m) {
    if (m->set.sim == 1) {
        modem_cms(m, MODEM_CMS_SIM_NOT_INSERTED);
        return false;
    }
    if (!modem_sim_ready(m)) {
        modem_cms(m, MODEM_CMS_SIM_PIN);
        return false;
    }
    return true;
}

static void cmd_cmgf(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    static const uint8_t allowed[] = { 0, 1 };
    struct sms *sms = m->sms;
    uint8_t text = sms->text;

    if (mode == MODEM_WRITE && a->count == 1 && a->v[0].str[0] == '\0') {
        sms->text = false;
        modem_ok(m);
        return;
    }
    modem_small_setting(m, mode, a, "+CMGF: ", &text, allowed, sizeof(allowed));
    sms->text = text != 0;
}

static void outbox_append(struct sms *sms, const char *p, size_t n) {
    for (size_t i = 0; i < n && sms->out_len < SMS_OUTBOX_SIZE - 1; i++) {
        sms->outbox[sms->out_len++] = p[i];
    }
}

static bool hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

// One byte of the message; true when it ended the data phase.
static bool sms_byte(struct modem *m, uint8_t b) {
    struct sms *sms = m->sms;
    struct text t;

    if (b == MODEM_ESC) {
        modem_phase_end(m);
        memset(sms->outbox, 0, sizeof(sms->outbox));
        sms->out_len = 0;
        modem_ok(m);
        return true;
    }
    if (b != MODEM_CTRL_Z) {
        if (!sms->text && !hex_digit((char)b)) {
            sms->pdu_ok = false;
        }
        char c = (char)b;
        outbox_append(sms, &c, 1);
        return false;
    }
    modem_phase_end(m);
    if (!sms->text && (!sms->pdu_ok || sms->out_len % 2 != 0 || sms->out_len == 0)) {
        modem_cms(m, MODEM_CMS_INVALID_PDU);
        return true;
    }
    sms->sent++;
    sms->mr++;
    text_init(&t);
    text_str(&t, "+CMGS: ");
    text_u32(&t, sms->mr);
    modem_answer_text(m, &t);
    return true;
}

static size_t cmgs_data(struct modem *m, const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (sms_byte(m, bytes[i])) {
            return i + 1;
        }
    }
    return len;
}

static const struct modem_phase cmgs_phase = {
    .data = cmgs_data,
    .complete = NULL,
    .expired = NULL,
};

// 27.005 3.5.1: text mode takes the number, PDU mode the TPDU length; both
// prompt and collect until Ctrl-Z, and ESC abandons the message.
static void cmd_cmgs(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sms *sms = m->sms;
    int32_t len;

    if (mode == MODEM_TEST) {
        modem_ok(m);
        return;
    }
    if (mode != MODEM_WRITE || a->count < 1) {
        modem_error(m);
        return;
    }
    if (!sms_allowed(m)) {
        return;
    }
    if (!modem_registered(m)) {
        modem_cms(m, MODEM_CMS_NO_NETWORK);
        return;
    }
    if (sms->text ? !a->v[0].quoted || a->v[0].str[0] == '\0' : !modem_arg_int(a, 0, 1, 255, &len)) {
        modem_cms(m, MODEM_CMS_NOT_ALLOWED);
        return;
    }
    memset(sms->outbox, 0, sizeof(sms->outbox));
    sms->out_len = 0;
    sms->pdu_ok = true;
    if (sms->text) {
        outbox_append(sms, a->v[0].str, strlen(a->v[0].str));
        outbox_append(sms, "\n", 1);
    }
    modem_prompt(m, &cmgs_phase, 0, 0, 0);
}

static uint8_t hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    return (uint8_t)((c | 0x20) - 'a' + 10);
}

// One stored message per line of the inbox region: its start and length,
// without the line end. Returns false past the last line.
static bool inbox_line(const struct sms *sms, size_t *pos, const char **start, size_t *len) {
    const char *inbox = sms->inbox;
    size_t p = *pos;

    if (p >= SMS_INBOX_SIZE || inbox[p] == '\0') {
        return false;
    }
    size_t end = p;
    while (end < SMS_INBOX_SIZE && inbox[end] != '\0' && inbox[end] != '\n') {
        end++;
    }
    *start = &inbox[p];
    *len = end - p;
    while (*len > 0 && (*start)[*len - 1] == '\r') {
        (*len)--;
    }
    *pos = end < SMS_INBOX_SIZE && inbox[end] == '\n' ? end + 1 : end;
    return true;
}

// 27.005 3.4.2, PDU mode: every stored message is "received read" (1), so
// <stat> 1 and 4 list them all and the others list nothing. The index is
// the inbox line number from 1.
static void cmd_cmgl(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    const struct sms *sms = m->sms;
    struct text t;
    int32_t stat;
    size_t pos = 0;
    const char *pdu;
    size_t len;
    bool any = false;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CMGL: (0-4)");
        return;
    }
    if (!sms_allowed(m)) {
        return;
    }
    if (sms->text) {
        modem_cms(m, MODEM_CMS_NOT_ALLOWED);
        return;
    }
    if (mode == MODEM_EXEC) {
        stat = 0;
    } else if (mode != MODEM_WRITE || !modem_arg_int(a, 0, 0, 4, &stat)) {
        modem_error(m);
        return;
    }
    for (uint32_t index = 1; index <= SMS_SLOTS && inbox_line(sms, &pos, &pdu, &len); index++) {
        if ((stat != 1 && stat != 4) || len < 2 || (sms->deleted & (1u << (index - 1))) != 0) {
            continue;
        }
        if (modem_out_free(m) < len + CMGL_ENTRY_OVERHEAD) {
            break;
        }
        uint32_t smsc = (uint32_t)(hex_value(pdu[0]) << 4 | hex_value(pdu[1]));
        uint32_t octets = (uint32_t)(len / 2);
        uint32_t tpdu = octets > smsc + 1 ? octets - smsc - 1 : 0;
        if (!any) {
            modem_out_str(m, "\r\n");
            any = true;
        }
        text_init(&t);
        text_str(&t, "+CMGL: ");
        text_u32(&t, index);
        text_str(&t, ",1,,");
        text_u32(&t, tpdu);
        modem_info_line(m, &t);
        modem_out(m, pdu, len);
        modem_out_str(m, "\r\n");
    }
    modem_ok(m);
}

static void cmd_cmgd(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    struct sms *sms = m->sms;
    int32_t index = 0;
    int32_t flag = 0;
    size_t pos = 0;
    const char *pdu;
    size_t len = 0;

    if (mode == MODEM_TEST) {
        modem_answer(m, "+CMGD: (1-32),(0-4)");
        return;
    }
    if (!sms_allowed(m)) {
        return;
    }
    if (mode != MODEM_WRITE || (a->count > 1 && !modem_arg_int(a, 1, 0, 4, &flag))) {
        modem_error(m);
        return;
    }
    if (flag != 0) {
        sms->deleted = UINT32_MAX;
        modem_ok(m);
        return;
    }
    if (!modem_arg_int(a, 0, 1, SMS_SLOTS, &index)) {
        modem_cms(m, MODEM_CMS_INVALID_INDEX);
        return;
    }
    for (int32_t i = 1; i <= index; i++) {
        if (!inbox_line(sms, &pos, &pdu, &len)) {
            modem_cms(m, MODEM_CMS_INVALID_INDEX);
            return;
        }
    }
    uint32_t bit = 1u << (index - 1);
    if (len < 2 || (sms->deleted & bit) != 0) {
        modem_cms(m, MODEM_CMS_INVALID_INDEX);
        return;
    }
    sms->deleted |= bit;
    modem_ok(m);
}

static void cmd_cscs(struct modem *m, enum modem_mode mode, struct modem_args *a) {
    (void)a;
    if (mode == MODEM_READ) {
        modem_answer(m, "+CSCS: \"IRA\"");
    } else if (mode == MODEM_TEST) {
        modem_answer(m, "+CSCS: (\"IRA\",\"GSM\",\"UCS2\")");
    } else {
        modem_ok(m);
    }
}

static const struct modem_command commands[] = {
    { "+CMGF", cmd_cmgf },
    { "+CSCS", cmd_cscs },
    { "+CSMP", modem_cmd_accept },
    { "+CNMI", modem_cmd_accept },
    { "+CMGS", cmd_cmgs },
    { "+CMGL", cmd_cmgl },
    { "+CMGD", cmd_cmgd },
};

const struct modem_commands sms_commands = MODEM_COMMANDS(commands);

static void sms_defaults(struct modem *m) {
    memset(m->sms->inbox, 0, sizeof(m->sms->inbox));
}

static void sms_clear(struct modem *m) {
    struct sms *sms = m->sms;

    sms->sent = 0;
    sms->deleted = 0;
    memset(sms->outbox, 0, sizeof(sms->outbox));
}

static void sms_session(struct modem *m) {
    struct sms *sms = m->sms;

    sms->out_len = 0;
    sms->mr = 0;
    sms->text = false;
    sms->pdu_ok = false;
}

static int sms_get(struct modem *m, size_t property, struct mclw_value *out) {
    if (property != 0) {
        return -EINVAL;
    }
    out->v[0] = m->sms->sent;
    return 0;
}

static void *sms_region(struct modem *m, size_t region) {
    switch (region) {
        case REGION_INBOX:
            return m->sms->inbox;
        case REGION_OUTBOX:
            return m->sms->outbox;
        default:
            return NULL;
    }
}

static void sms_written(struct modem *m, size_t region) {
    if (region == REGION_INBOX) {
        m->sms->deleted = 0;
    }
}

const struct modem_part sms_part = {
    .property_count = SMS_PROPERTY_COUNT,
    .region_count = SMS_REGION_COUNT,
    .defaults = sms_defaults,
    .clear = sms_clear,
    .session = sms_session,
    .get = sms_get,
    .set = NULL,
    .region = sms_region,
    .written = sms_written,
};
