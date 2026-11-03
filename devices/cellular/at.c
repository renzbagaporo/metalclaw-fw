// SPDX-License-Identifier: Apache-2.0 OR MIT
// The AT engine: command lines and data phases out of a channel's byte
// stream, dispatch over the model's tables, V.250 result framing and
// +CME/+CMS errors.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/runtime.h"

#include "modem.h"
#include "text.h"

struct error_text {
    uint16_t code;
    const char *text;
};

static const struct error_text cme_texts[] = {
    { MODEM_CME_NOT_ALLOWED, "operation not allowed" },
    { MODEM_CME_NOT_SUPPORTED, "operation not supported" },
    { MODEM_CME_SIM_NOT_INSERTED, "SIM not inserted" },
    { MODEM_CME_SIM_PIN_REQUIRED, "SIM PIN required" },
    { MODEM_CME_NO_NETWORK, "no network service" },
};

static const struct error_text cms_texts[] = {
    { MODEM_CMS_NOT_ALLOWED, "operation not allowed" },
    { MODEM_CMS_INVALID_PDU, "invalid PDU mode" },
    { MODEM_CMS_SIM_NOT_INSERTED, "SIM not inserted" },
    { MODEM_CMS_SIM_PIN, "SIM pin necessary" },
    { MODEM_CMS_INVALID_INDEX, "invalid memory index" },
    { MODEM_CMS_NO_NETWORK, "no network" },
};

void modem_result(struct modem *m, uint8_t code, const char *text) {
    struct text t;

    text_init(&t);
    if (m->chan->numeric) {
        text_u32(&t, code);
        text_char(&t, '\r');
    } else {
        text_str(&t, "\r\n");
        text_str(&t, text);
        text_str(&t, "\r\n");
    }
    modem_out_text(m, &t);
}

void modem_ok(struct modem *m) {
    if (m->chan->numeric) {
        modem_out_str(m, "0\r");
    } else {
        modem_out_str(m, "\r\nOK\r\n");
    }
}

void modem_error(struct modem *m) {
    if (m->chan->numeric) {
        modem_out_str(m, "4\r");
    } else {
        modem_out_str(m, "\r\nERROR\r\n");
    }
}

static void error_code(struct modem *m, const char *prefix, const struct error_text *texts, size_t count,
    uint16_t code) {
    struct text t;
    const char *str = NULL;

    if (m->ses.cmee == 0) {
        modem_error(m);
        return;
    }
    for (size_t i = 0; i < count && m->ses.cmee == 2; i++) {
        if (texts[i].code == code) {
            str = texts[i].text;
        }
    }
    text_init(&t);
    text_str(&t, prefix);
    if (str != NULL) {
        text_str(&t, str);
    } else {
        text_u32(&t, code);
    }
    modem_info(m, &t);
}

void modem_cme(struct modem *m, uint16_t code) {
    error_code(m, "+CME ERROR: ", cme_texts, sizeof(cme_texts) / sizeof(cme_texts[0]), code);
}

void modem_cms(struct modem *m, uint16_t code) {
    error_code(m, "+CMS ERROR: ", cms_texts, sizeof(cms_texts) / sizeof(cms_texts[0]), code);
}

void modem_info(struct modem *m, const struct text *t) {
    if (!m->chan->numeric) {
        modem_out_str(m, "\r\n");
    }
    modem_info_line(m, t);
}

void modem_info_str(struct modem *m, const char *str) {
    if (!m->chan->numeric) {
        modem_out_str(m, "\r\n");
    }
    modem_out_str(m, str);
    modem_out_str(m, "\r\n");
}

void modem_info_line(struct modem *m, const struct text *t) {
    modem_out_text(m, t);
    modem_out_str(m, "\r\n");
}

void modem_answer(struct modem *m, const char *line) {
    modem_info_str(m, line);
    modem_ok(m);
}

void modem_answer_text(struct modem *m, const struct text *t) {
    modem_info(m, t);
    modem_ok(m);
}

void modem_answer_u32(struct modem *m, const char *prefix, uint32_t v) {
    struct text t;

    text_init(&t);
    text_str(&t, prefix);
    text_u32(&t, v);
    modem_answer_text(m, &t);
}

void modem_prompt(struct modem *m, const struct modem_phase *phase, uint16_t len, uint8_t arg, uint32_t timeout_ms) {
    struct modem_chan *ch = m->chan;

    modem_out_str(m, "\r\n> ");
    ch->phase = phase;
    ch->data_left = len;
    ch->data_arg = arg;
    m->ses.data_arg = arg;
    ch->data_deadline_us = timeout_ms == 0 ? 0 : modem_now() + (uint64_t)timeout_ms * 1000u;
}

void modem_phase_end(struct modem *m) {
    m->chan->phase = NULL;
}

void modem_defer_ok(struct modem *m, uint32_t delay_us) {
    m->chan->final_pending = true;
    m->chan->final_due_us = modem_now() + delay_us;
}

static bool parse_int(const char *p, int32_t *out) {
    bool negative = false;
    uint32_t v = 0;
    size_t digits = 0;

    if (*p == '-' || *p == '+') {
        negative = *p == '-';
        p++;
    }
    for (; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || digits == 9) {
            return false;
        }
        v = v * 10u + (uint32_t)(*p - '0');
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    *out = negative ? -(int32_t)v : (int32_t)v;
    return true;
}

static void parse_args(char *p, struct modem_args *a) {
    a->count = 0;
    while (a->count < MODEM_ARGS_MAX) {
        struct modem_arg *arg = &a->v[a->count++];
        while (*p == ' ') {
            p++;
        }
        arg->quoted = *p == '"';
        arg->is_num = false;
        arg->num = 0;
        if (arg->quoted) {
            p++;
            arg->str = p;
            while (*p != '\0' && *p != '"') {
                p++;
            }
            if (*p == '"') {
                *p++ = '\0';
            }
            while (*p != '\0' && *p != ',') {
                p++;
            }
        } else {
            arg->str = p;
            while (*p != '\0' && *p != ',') {
                p++;
            }
        }
        char sep = *p;
        if (sep != '\0') {
            *p++ = '\0';
        }
        if (!arg->quoted) {
            size_t n = strlen(arg->str);
            while (n > 0 && arg->str[n - 1] == ' ') {
                arg->str[--n] = '\0';
            }
            arg->is_num = parse_int(arg->str, &arg->num);
        }
        if (sep == '\0') {
            return;
        }
    }
}

bool modem_arg_int(const struct modem_args *a, uint8_t i, int32_t lo, int32_t hi, int32_t *out) {
    if (i >= a->count || !a->v[i].is_num || a->v[i].num < lo || a->v[i].num > hi) {
        return false;
    }
    *out = a->v[i].num;
    return true;
}

const char *modem_arg_str(const struct modem_args *a, uint8_t i) {
    return i < a->count ? a->v[i].str : NULL;
}

uint32_t modem_fourcc(const char *name) {
    uint32_t v = 0;

    for (size_t i = 0; i < 4 && name[i] != '\0'; i++) {
        v = (v << 8) | (uint8_t)name[i];
    }
    return v;
}

static const struct modem_command *find_command(const struct modem *m, const char *name) {
    const struct modem_config *c = m->config;

    for (size_t t = 0; t < c->table_count; t++) {
        const struct modem_commands *table = c->tables[t];
        for (size_t i = 0; i < table->count; i++) {
            if (strcmp(table->list[i].name, name) == 0) {
                return table->list[i].fn != NULL ? &table->list[i] : NULL;
            }
        }
    }
    return NULL;
}

static void unknown(struct modem *m, const char *name) {
    m->stats.unknown_commands++;
    modem_trace(m, MCLW_TRACE_ERROR, modem_fourcc(name), 0, -ENOTSUP);
    modem_error(m);
}

// V.250 commands without a `+`: only the forms the DUT drivers send, and
// D for a table that maps it.
static void basic(struct modem *m, char *p) {
    for (char *q = p; *q != '\0'; q++) {
        *q = text_upper(*q);
    }
    const struct modem_command *dial = p[0] == 'D' ? find_command(m, "D") : NULL;
    if (dial != NULL) {
        struct modem_args args;
        args.count = 1;
        args.v[0].str = &p[1];
        args.v[0].num = 0;
        args.v[0].is_num = false;
        args.v[0].quoted = false;
        modem_trace(m, MCLW_TRACE_COMMAND, modem_fourcc(p), MODEM_EXEC, 0);
        dial->fn(m, MODEM_EXEC, &args);
    } else if (strcmp(p, "E") == 0 || strcmp(p, "E0") == 0) {
        m->chan->echo = false;
        modem_ok(m);
    } else if (strcmp(p, "E1") == 0) {
        m->chan->echo = true;
        modem_ok(m);
    } else if (strcmp(p, "I") == 0 || strcmp(p, "I0") == 0) {
        modem_answer(m, m->config->product);
    } else if (strcmp(p, "V0") == 0) {
        m->chan->numeric = true;
        modem_ok(m);
    } else if (strcmp(p, "V") == 0 || strcmp(p, "V1") == 0) {
        m->chan->numeric = false;
        modem_ok(m);
    } else if (strcmp(p, "Z") == 0 || strcmp(p, "Z0") == 0 || strcmp(p, "&F") == 0 ||
        strcmp(p, "&F0") == 0 || strcmp(p, "&W") == 0) {
        modem_ok(m);
    } else {
        unknown(m, p);
    }
}

static void extended(struct modem *m, char *p) {
    struct modem_args args;
    enum modem_mode mode = MODEM_EXEC;
    size_t n = 0;

    while (p[n] != '\0' && p[n] != '=' && p[n] != '?') {
        p[n] = text_upper(p[n]);
        n++;
    }
    args.count = 0;
    if (p[n] == '?') {
        mode = MODEM_READ;
    } else if (p[n] == '=' && p[n + 1] == '?') {
        mode = MODEM_TEST;
    } else if (p[n] == '=') {
        mode = MODEM_WRITE;
        parse_args(&p[n + 1], &args);
    }
    p[n] = '\0';

    const struct modem_command *cmd = find_command(m, p);
    if (cmd == NULL) {
        unknown(m, p + 1);
        return;
    }
    modem_trace(m, MCLW_TRACE_COMMAND, modem_fourcc(p + 1), (uint16_t)mode, 0);
    cmd->fn(m, mode, &args);
}

// One command per line: `;` concatenation is not modelled.
static void execute(struct modem *m) {
    struct modem_chan *ch = m->chan;
    size_t len = ch->line_len;
    bool overflow = ch->line_overflow;
    char *line = ch->line;

    line[len] = '\0';
    ch->line_len = 0;
    ch->line_overflow = false;
    if (len < 2 || text_upper(line[1]) != 'T') {
        return;
    }
    m->stats.commands++;
    if (ch->echo) {
        modem_out(m, line, len);
        modem_out_str(m, "\r");
    }
    if (overflow) {
        modem_error(m);
    } else if (line[2] == '\0') {
        modem_ok(m);
    } else if (line[2] == '+' || line[2] == '^') {
        extended(m, &line[2]);
    } else {
        basic(m, &line[2]);
    }
    modem_pump(m);
}

// Anything before an `A` is noise (the LF after a CR, spaces, a NUL on
// wake-up) and never opens a line, so it never holds back a URC.
static void command_byte(struct modem *m, uint8_t b) {
    struct modem_chan *ch = m->chan;

    if (b == '\r') {
        if (ch->line_len > 0) {
            execute(m);
        }
        ch->swallow_lf = true;
        return;
    }
    if (ch->line_len == 0 && b != 'A' && b != 'a') {
        return;
    }
    if (ch->line_len >= MODEM_LINE_MAX) {
        ch->line_overflow = true;
        return;
    }
    ch->line[ch->line_len++] = (char)b;
}

// A command waits while the last one's final result is pending or the
// output FIFO could not take an answer.
static bool must_wait(const struct modem *m) {
    return m->chan->final_pending || modem_out_free(m) < m->config->command_room;
}

static void stash(struct modem *m, const uint8_t *bytes, size_t len) {
    struct modem_chan *ch = m->chan;
    size_t room = MODEM_STASH - ch->stash_len;
    size_t n = len < room ? len : room;

    memcpy(&ch->stash[ch->stash_len], bytes, n);
    ch->stash_len = (uint8_t)(ch->stash_len + n);
    if (n < len) {
        modem_trace(m, MCLW_TRACE_ERROR, modem_fourcc("STSH"), (uint16_t)(len - n), -ENOSPC);
    }
}

// Hands a data phase what is for it, and ends a counted one at its last
// byte.
static size_t phase_data(struct modem *m, const uint8_t *bytes, size_t len) {
    struct modem_chan *ch = m->chan;
    const struct modem_phase *phase = ch->phase;

    m->ses.data_arg = ch->data_arg;
    if (ch->data_left == 0) {
        return phase->data(m, bytes, len);
    }
    if (len > ch->data_left) {
        len = ch->data_left;
    }
    phase->data(m, bytes, len);
    ch->data_left = (uint16_t)(ch->data_left - len);
    if (ch->data_left == 0) {
        ch->phase = NULL;
        phase->complete(m);
    }
    return len;
}

// The channel stopped taking AT: a dial turned it to PPP, or AT+CMUX
// started the multiplexer under it.
static bool left_command_mode(const struct modem *m, const struct modem_chan *ch) {
    return ch->mode != MODEM_CHAN_COMMAND || (ch->dlci == 0 && m->link != NULL && m->link->cmux);
}

// The LF that follows a line's CR is dropped even when the line opened a
// data phase: TinyGSM and Zephyr both end `AT+CASEND=<cid>,<len>` with
// CR LF, and that LF is not payload.
static size_t consume(struct modem *m, const uint8_t *bytes, size_t len) {
    struct modem_chan *ch = m->chan;
    size_t i = 0;

    while (i < len && m->power == MODEM_ON) {
        if (left_command_mode(m, ch)) {
            return i;
        }
        if (ch->phase == NULL && must_wait(m)) {
            stash(m, &bytes[i], len - i);
            return len;
        }
        if (ch->swallow_lf) {
            ch->swallow_lf = false;
            if (bytes[i] == '\n') {
                i++;
                continue;
            }
        }
        if (ch->phase != NULL) {
            i += phase_data(m, &bytes[i], len - i);
        } else {
            command_byte(m, bytes[i]);
            i++;
        }
    }
    return len;
}

size_t modem_chan_rx(struct modem *m, struct modem_chan *ch, const uint8_t *bytes, size_t len) {
    size_t done;

    modem_select(m, ch);
    if (ch->mode == MODEM_CHAN_PPP) {
        ppp_rx(&m->link->ppp, bytes, len, modem_now());
        return len;
    }
    if (ch->mode != MODEM_CHAN_COMMAND) {
        return len;
    }
    if (ch->stash_len > 0) {
        stash(m, bytes, len);
        modem_at_resume(m);
        return len;
    }
    done = consume(m, bytes, len);
    // A dial hands the rest of the chunk to PPP.
    if (done < len && ch->mode == MODEM_CHAN_PPP) {
        ppp_rx(&m->link->ppp, &bytes[done], len - done, modem_now());
        done = len;
    }
    return done;
}

void modem_at_rx(struct modem *m, const uint8_t *bytes, size_t len) {
    modem_chan_rx(m, &m->ses.raw, bytes, len);
}

void modem_at_resume(struct modem *m) {
    struct modem_chan *ch = m->chan;
    uint8_t held[MODEM_STASH];
    size_t n = ch->stash_len;

    if (n == 0 || m->power != MODEM_ON || ch->mode != MODEM_CHAN_COMMAND || must_wait(m)) {
        return;
    }
    memcpy(held, ch->stash, n);
    ch->stash_len = 0;
    size_t done = consume(m, held, n);
    if (done < n && ch->mode == MODEM_CHAN_PPP) {
        ppp_rx(&m->link->ppp, &held[done], n - done, modem_now());
    }
}

void modem_at_tick(struct modem *m, uint64_t now_us) {
    for (size_t i = 0; m->power == MODEM_ON; i++) {
        struct modem_chan *ch = modem_chan_at(m, i);
        if (ch == NULL) {
            return;
        }
        const struct modem_phase *phase = ch->phase;
        if (phase == NULL || ch->data_deadline_us == 0 || now_us < ch->data_deadline_us) {
            continue;
        }
        modem_select(m, ch);
        ch->phase = NULL;
        ch->data_left = 0;
        m->ses.data_arg = ch->data_arg;
        phase->expired(m);
    }
}
