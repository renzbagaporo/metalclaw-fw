// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cmux.h"

enum {
    RX_FLAG,
    RX_ADDR,
    RX_CTRL,
    RX_LEN,
    RX_DATA,
    RX_FCS,
    RX_END,
};

// A control message the responder echoes back: type, length, at most a
// few value octets (an MSC with break is 5 in all).
#define MSG_MAX 8u

// CRC-8/ROHC, reflected polynomial 0xE0 (27.010 5.2.1.6); Zephyr's
// crc8_rohc.
static uint8_t crc_byte(uint8_t f, uint8_t b) {
    f ^= b;
    for (int i = 0; i < 8; i++) {
        f = (f & 1u) != 0 ? (uint8_t)((f >> 1) ^ 0xe0u) : (uint8_t)(f >> 1);
    }
    return f;
}

uint8_t cmux_fcs(const uint8_t *p, size_t len) {
    uint8_t f = 0xffu;

    for (size_t i = 0; i < len; i++) {
        f = crc_byte(f, p[i]);
    }
    return (uint8_t)(0xffu - f);
}

size_t cmux_encode(uint8_t *out, uint8_t dlci, bool cr, uint8_t ctrl, const uint8_t *data, size_t len) {
    size_t n = 0;
    uint8_t f = 0xffu;

    out[n++] = CMUX_FLAG;
    out[n++] = (uint8_t)((dlci << 2) | (cr ? CMUX_CR : 0u) | CMUX_EA);
    out[n++] = ctrl;
    out[n++] = (uint8_t)((len << 1) | CMUX_EA);
    for (size_t i = 1; i < 4; i++) {
        f = crc_byte(f, out[i]);
    }
    // 27.010 5.3.6: the FCS of a UIH frame leaves the information field
    // out; every other frame type includes it.
    if ((ctrl & (uint8_t)~CMUX_PF) != CMUX_UIH) {
        for (size_t i = 0; i < len; i++) {
            f = crc_byte(f, data[i]);
        }
    }
    if (len > 0) {
        memcpy(&out[n], data, len);
        n += len;
    }
    out[n++] = (uint8_t)(0xffu - f);
    out[n++] = CMUX_FLAG;
    return n;
}

void cmux_start(struct cmux *c, uint8_t n1) {
    struct cmux_stats keep = c->stats;

    memset(c, 0, sizeof(*c));
    c->stats = keep;
    c->state = RX_FLAG;
    c->n1 = n1 == 0 ? 1 : (n1 > CMUX_MTU ? (uint8_t)CMUX_MTU : n1);
}

bool cmux_is_open(const struct cmux *c, uint8_t dlci) {
    return dlci < 8 && (c->open & (1u << dlci)) != 0;
}

bool cmux_can_send(const struct cmux *c, uint8_t dlci) {
    return cmux_is_open(c, dlci) && !c->all_stopped && (c->stopped & (1u << dlci)) == 0;
}

// A reply from the responder: C/R 1.
static void reply(const struct cmux_sink *sink, void *ctx, uint8_t dlci, uint8_t ctrl, const uint8_t *data,
    size_t len) {
    uint8_t frame[CMUX_OVERHEAD + MSG_MAX];

    sink->send(ctx, frame, cmux_encode(frame, dlci, true, ctrl, data, len));
}

// One control message on DLCI 0. Returns true when it closed the
// multiplexer.
static bool control_message(struct cmux *c, const uint8_t *msg, size_t len, const struct cmux_sink *sink,
    void *ctx) {
    uint8_t answer[MSG_MAX];

    if (len < 2 || (msg[0] & CMUX_EA) == 0 || (msg[1] & CMUX_EA) == 0) {
        return false;
    }
    uint8_t type = msg[0] >> 2;
    size_t value_len = msg[1] >> 1;
    const uint8_t *value = &msg[2];
    if ((msg[0] & CMUX_CR) == 0 || 2u + value_len > len || 2u + value_len > MSG_MAX) {
        return false;
    }
    memcpy(answer, msg, 2u + value_len);
    answer[0] = (uint8_t)(msg[0] & (uint8_t)~CMUX_CR);
    switch (type) {
        case CMUX_MSG_MSC:
            if (value_len >= 2) {
                uint8_t dlci = value[0] >> 2;
                bool stop = (value[1] & CMUX_MSC_FC) != 0 || (value[1] & CMUX_MSC_RTR) == 0;
                if (dlci < 8) {
                    c->stopped = stop ? (uint8_t)(c->stopped | (1u << dlci)) : (uint8_t)(c->stopped & ~(1u << dlci));
                }
            }
            break;
        case CMUX_MSG_FCON:
            c->all_stopped = false;
            break;
        case CMUX_MSG_FCOFF:
            c->all_stopped = true;
            break;
        case CMUX_MSG_CLD:
        case CMUX_MSG_TEST:
        case CMUX_MSG_PSC:
            break;
        default:
            answer[0] = (uint8_t)((CMUX_MSG_NSC << 2) | CMUX_EA);
            answer[1] = (uint8_t)((1u << 1) | CMUX_EA);
            answer[2] = msg[0];
            value_len = 1;
            break;
    }
    reply(sink, ctx, 0, CMUX_UIH, answer, 2u + value_len);
    return type == CMUX_MSG_CLD;
}

// Returns true when the frame closed the multiplexer.
static bool frame(struct cmux *c, const struct cmux_sink *sink, void *ctx) {
    uint8_t dlci = c->addr >> 2;
    uint8_t type = (uint8_t)(c->ctrl & (uint8_t)~CMUX_PF);
    uint8_t bit = (uint8_t)(dlci < 8 ? 1u << dlci : 0u);

    if (dlci == 0) {
        switch (type) {
            case CMUX_SABM:
                c->open |= 1u;
                reply(sink, ctx, 0, CMUX_UA | CMUX_PF, NULL, 0);
                return false;
            case CMUX_DISC:
                reply(sink, ctx, 0, CMUX_UA | CMUX_PF, NULL, 0);
                return true;
            case CMUX_UIH:
            case CMUX_UI:
                return control_message(c, c->buf, c->len, sink, ctx);
            default:
                return false;
        }
    }
    switch (type) {
        case CMUX_SABM:
            if ((c->open & bit) != 0) {
                reply(sink, ctx, dlci, CMUX_UA | CMUX_PF, NULL, 0);
            } else if (dlci <= CMUX_DLCI_MAX && (c->open & 1u) != 0 && sink->open(ctx, dlci)) {
                c->open |= bit;
                c->stopped = (uint8_t)(c->stopped & ~bit);
                reply(sink, ctx, dlci, CMUX_UA | CMUX_PF, NULL, 0);
            } else {
                reply(sink, ctx, dlci, CMUX_DM | CMUX_PF, NULL, 0);
            }
            return false;
        case CMUX_DISC:
            reply(sink, ctx, dlci, CMUX_UA | CMUX_PF, NULL, 0);
            if ((c->open & bit) != 0) {
                c->open = (uint8_t)(c->open & ~bit);
                c->stopped = (uint8_t)(c->stopped & ~bit);
                sink->close(ctx, dlci);
            }
            return false;
        case CMUX_UIH:
        case CMUX_UI:
            if ((c->open & bit) != 0 && c->len > 0) {
                sink->data(ctx, dlci, c->buf, c->len);
            }
            return false;
        default:
            return false;
    }
}

size_t cmux_input(struct cmux *c, const uint8_t *bytes, size_t len, const struct cmux_sink *sink, void *ctx) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = bytes[i];
        switch (c->state) {
            case RX_FLAG:
                if (b == CMUX_FLAG) {
                    c->state = RX_ADDR;
                }
                break;
            case RX_ADDR:
                // Any run of flags is a resync; a two-octet address is not
                // modelled.
                if (b == CMUX_FLAG) {
                    break;
                }
                c->addr = b;
                c->fcs = crc_byte(0xffu, b);
                c->state = (b & CMUX_EA) != 0 ? RX_CTRL : RX_FLAG;
                break;
            case RX_CTRL:
                c->ctrl = b;
                c->fcs = crc_byte(c->fcs, b);
                c->state = RX_LEN;
                break;
            case RX_LEN: {
                uint8_t type = (uint8_t)(c->ctrl & (uint8_t)~CMUX_PF);
                c->len = b >> 1;
                c->pos = 0;
                c->fcs = crc_byte(c->fcs, b);
                // A two-octet length is longer than Zephyr's MTU; payload
                // rides on UIH and UI only.
                if ((b & CMUX_EA) == 0 || (c->len > 0 && type != CMUX_UIH && type != CMUX_UI)) {
                    c->state = RX_FLAG;
                } else {
                    c->state = c->len > 0 ? RX_DATA : RX_FCS;
                }
                break;
            }
            case RX_DATA:
                c->buf[c->pos++] = b;
                if ((c->ctrl & (uint8_t)~CMUX_PF) != CMUX_UIH) {
                    c->fcs = crc_byte(c->fcs, b);
                }
                if (c->pos == c->len) {
                    c->state = RX_FCS;
                }
                break;
            case RX_FCS:
                if ((uint8_t)(0xffu - c->fcs) == b) {
                    c->state = RX_END;
                } else {
                    c->stats.bad_fcs++;
                    c->state = RX_FLAG;
                }
                break;
            default:
                if (b != CMUX_FLAG) {
                    c->state = RX_FLAG;
                    break;
                }
                // The closing flag may open the next frame too.
                c->state = RX_ADDR;
                if (frame(c, sink, ctx)) {
                    c->open = 0;
                    c->state = RX_FLAG;
                    sink->closedown(ctx);
                    return i + 1;
                }
                break;
        }
    }
    return len;
}
