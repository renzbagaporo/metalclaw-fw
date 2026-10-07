// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/bus.h"
#include "metalclaw/hal/uart.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "neo_m8n.h"

enum {
    PROP_LATITUDE,
    PROP_LONGITUDE,
    PROP_ALTITUDE,
    PROP_SPEED,
    PROP_HEADING,
    PROP_SATELLITES,
    PROP_FIX,
    PROP_PDOP,
    PROP_TIME,
    PROP_BAUD,
    PROP_RATE_MS,
    PROP_NAV_PVT_RATE,
    PROP_DYN_MODEL,
    PROP_RUNNING,
    PROP_RESETS,
    PROP_FIXES,
};

enum {
    BINDING_UART,
};

enum parse_state {
    PARSE_SYNC_1,
    PARSE_SYNC_2,
    PARSE_CLASS,
    PARSE_ID,
    PARSE_LEN_LO,
    PARSE_LEN_HI,
    PARSE_PAYLOAD,
    PARSE_CK_A,
    PARSE_CK_B,
};

struct neo_m8n_state {
    struct mclw_uart *uart;
    int32_t latitude;
    int32_t longitude;
    int32_t altitude;
    uint32_t speed;
    int32_t heading;
    uint8_t satellites;
    uint8_t fix;
    uint16_t pdop;
    int32_t time[6];
    uint32_t baud;
    uint16_t rate_ms;
    uint16_t nav_rate;
    uint16_t time_ref;
    uint8_t nav_pvt_rate;
    uint8_t dyn_model;
    bool running;
    uint32_t resets;
    uint32_t fixes;
    uint64_t next_fix_us;
    enum parse_state parse;
    uint8_t cls;
    uint8_t id;
    uint16_t len;
    uint16_t got;
    uint8_t ck_a;
    uint8_t ck_b;
    bool oversize;
    uint8_t payload[UBX_PAYLOAD_MAX];
    uint8_t frame[UBX_HEADER_BYTES + UBX_NAV_PVT_LEN + UBX_TRAILER_BYTES];
};

static const struct mclw_property_desc neo_m8n_properties[] = {
    {
        .name = "latitude",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "1e-7 deg",
        .min = -NEO_M8N_LATITUDE_MAX,
        .max = NEO_M8N_LATITUDE_MAX,
    },
    {
        .name = "longitude",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "1e-7 deg",
        .min = -NEO_M8N_LONGITUDE_MAX,
        .max = NEO_M8N_LONGITUDE_MAX,
    },
    {
        .name = "altitude",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mm",
        .min = INT32_MIN,
        .max = INT32_MAX,
    },
    {
        .name = "speed",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mm/s",
        .min = 0,
        .max = INT32_MAX,
    },
    {
        .name = "heading",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "1e-5 deg",
        .min = 0,
        .max = NEO_M8N_HEADING_MAX,
    },
    {
        .name = "satellites",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 0,
        .max = NEO_M8N_SATELLITES_MAX,
    },
    {
        .name = "fix",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 0,
        .max = 5,
    },
    {
        .name = "pdop",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "0.01",
        .min = 0,
        .max = UINT16_MAX,
    },
    {
        .name = "time",
        .type = MCLW_TYPE_I32,
        .count = 6,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 0,
        .max = 9999,
    },
    {
        .name = "baud",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "baud",
        .min = NEO_M8N_BAUD_MIN,
        .max = NEO_M8N_BAUD_MAX,
    },
    {
        .name = "rate_ms",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = "ms",
        .min = 0,
        .max = NEO_M8N_RATE_MS_MAX,
    },
    {
        .name = "nav_pvt_rate",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 255,
    },
    {
        .name = "dyn_model",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 255,
    },
    {
        .name = "running",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    {
        .name = "resets",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "fixes",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
};

static const struct mclw_binding_desc neo_m8n_bindings[] = {
    {
        .name = "uart",
        .kind = MCLW_BINDING_UART,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = {
            .default_baud = NEO_M8N_DEFAULT_BAUD,
            .format = {
                .data_bits = 8,
                .stop_bits = 1,
                .parity = MCLW_UART_PARITY_NONE,
            },
        },
    },
};

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Frames `payload` as UBX in state->frame and queues it. The checksum is
// 8-bit Fletcher over class, id, length and payload.
static void send_frame(struct neo_m8n_state *state, uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
    uint8_t *f = state->frame;
    uint8_t ck_a = 0;
    uint8_t ck_b = 0;
    size_t total = UBX_HEADER_BYTES + len + UBX_TRAILER_BYTES;

    if (total > sizeof(state->frame)) {
        return;
    }
    f[0] = UBX_SYNC_1;
    f[1] = UBX_SYNC_2;
    f[2] = cls;
    f[3] = id;
    put_u16(&f[4], len);
    if (len != 0) {
        memcpy(&f[UBX_HEADER_BYTES], payload, len);
    }
    for (size_t i = 2; i < (size_t)UBX_HEADER_BYTES + len; i++) {
        ck_a = (uint8_t)(ck_a + f[i]);
        ck_b = (uint8_t)(ck_b + ck_a);
    }
    f[UBX_HEADER_BYTES + len] = ck_a;
    f[UBX_HEADER_BYTES + len + 1] = ck_b;
    if (mclw_hal_uart_send(state->uart, f, total) != total) {
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_ERROR, ((uint32_t)cls << 8) | id, (uint16_t)total, -ENOSPC);
    } else {
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_UART, ((uint32_t)cls << 8) | id, (uint16_t)total, 0);
    }
}

static void send_ack(struct neo_m8n_state *state, bool ack, uint8_t cls, uint8_t id) {
    uint8_t payload[2] = { cls, id };

    send_frame(state, UBX_CLASS_ACK, ack ? UBX_ACK_ACK : UBX_ACK_NAK, payload, sizeof(payload));
}

static void send_mon_ver(struct neo_m8n_state *state) {
    uint8_t payload[UBX_MON_VER_LEN];

    memset(payload, 0, sizeof(payload));
    memcpy(payload, NEO_M8N_SW_VERSION, sizeof(NEO_M8N_SW_VERSION));
    memcpy(&payload[30], NEO_M8N_HW_VERSION, sizeof(NEO_M8N_HW_VERSION));
    send_frame(state, UBX_CLASS_MON, UBX_MON_VER, payload, sizeof(payload));
}

static void send_mon_gnss(struct neo_m8n_state *state) {
    uint8_t payload[UBX_MON_GNSS_LEN];

    memset(payload, 0, sizeof(payload));
    payload[1] = UBX_GNSS_GPS | UBX_GNSS_GLONASS | UBX_GNSS_BEIDOU | UBX_GNSS_GALILEO;
    payload[2] = UBX_GNSS_GPS | UBX_GNSS_GLONASS;
    payload[3] = UBX_GNSS_GPS | UBX_GNSS_GLONASS;
    payload[4] = 2;
    send_frame(state, UBX_CLASS_MON, UBX_MON_GNSS, payload, sizeof(payload));
}

static void send_cfg_rate(struct neo_m8n_state *state) {
    uint8_t payload[UBX_CFG_RATE_LEN];

    put_u16(&payload[0], state->rate_ms);
    put_u16(&payload[2], state->nav_rate);
    put_u16(&payload[4], state->time_ref);
    send_frame(state, UBX_CLASS_CFG, UBX_CFG_RATE, payload, sizeof(payload));
}

static void send_cfg_nav5(struct neo_m8n_state *state) {
    uint8_t payload[UBX_CFG_NAV5_LEN];

    memset(payload, 0, sizeof(payload));
    put_u16(&payload[0], 0xffff);
    payload[2] = state->dyn_model;
    payload[3] = 3;
    send_frame(state, UBX_CLASS_CFG, UBX_CFG_NAV5, payload, sizeof(payload));
}

static void send_cfg_prt(struct neo_m8n_state *state) {
    uint8_t payload[UBX_CFG_PRT_LEN];

    memset(payload, 0, sizeof(payload));
    payload[0] = UBX_CFG_PRT_PORT_UART1;
    put_u32(&payload[4], 0x000008c0);
    put_u32(&payload[8], state->baud);
    put_u16(&payload[12], 0x0001);
    put_u16(&payload[14], 0x0001);
    send_frame(state, UBX_CLASS_CFG, UBX_CFG_PRT, payload, sizeof(payload));
}

static void send_nav_pvt(struct neo_m8n_state *state) {
    uint8_t p[UBX_NAV_PVT_LEN];
    uint8_t valid = 0;
    uint8_t flags = 0;

    memset(p, 0, sizeof(p));
    if (state->fix != UBX_NAV_PVT_FIX_NONE) {
        valid = UBX_NAV_PVT_VALID_DATE | UBX_NAV_PVT_VALID_TIME | UBX_NAV_PVT_VALID_FULLY_RESOLVED;
        flags = UBX_NAV_PVT_FLAGS_GNSS_FIX_OK;
    }
    put_u32(&p[0], (uint32_t)(state->fixes * (uint32_t)state->rate_ms));
    put_u16(&p[4], (uint16_t)state->time[0]);
    p[6] = (uint8_t)state->time[1];
    p[7] = (uint8_t)state->time[2];
    p[8] = (uint8_t)state->time[3];
    p[9] = (uint8_t)state->time[4];
    p[10] = (uint8_t)state->time[5];
    p[11] = valid;
    p[20] = state->fix;
    p[21] = flags;
    p[23] = state->satellites;
    put_u32(&p[24], (uint32_t)state->longitude);
    put_u32(&p[28], (uint32_t)state->latitude);
    put_u32(&p[32], (uint32_t)state->altitude);
    put_u32(&p[36], (uint32_t)state->altitude);
    put_u32(&p[60], state->speed);
    put_u32(&p[64], (uint32_t)state->heading);
    put_u16(&p[76], state->pdop);
    put_u32(&p[84], (uint32_t)state->heading);
    send_frame(state, UBX_CLASS_NAV, UBX_NAV_PVT, p, sizeof(p));
    state->fixes++;
}

static void apply_baud(struct neo_m8n_state *state, uint32_t baud) {
    if (mclw_hal_uart_set_baud(state->uart, baud) == 0) {
        state->baud = baud;
    }
}

static void handle_cfg(struct neo_m8n_state *state) {
    const uint8_t *p = state->payload;

    switch (state->id) {
        case UBX_CFG_PRT:
            if (state->len == 0 || state->len == 1) {
                send_cfg_prt(state);
            } else if (state->len == UBX_CFG_PRT_LEN && p[0] == UBX_CFG_PRT_PORT_UART1) {
                uint32_t baud = get_u32(&p[8]);

                send_ack(state, true, state->cls, state->id);
                if (baud >= NEO_M8N_BAUD_MIN && baud <= NEO_M8N_BAUD_MAX) {
                    apply_baud(state, baud);
                }
            } else {
                send_ack(state, false, state->cls, state->id);
            }
            break;
        case UBX_CFG_MSG:
            if (state->len == UBX_CFG_MSG_LEN) {
                if (p[0] == UBX_CLASS_NAV && p[1] == UBX_NAV_PVT) {
                    state->nav_pvt_rate = p[2];
                }
                send_ack(state, true, state->cls, state->id);
            } else {
                send_ack(state, false, state->cls, state->id);
            }
            break;
        case UBX_CFG_RST:
            if (state->len == UBX_CFG_RST_LEN) {
                if (p[2] == UBX_CFG_RST_STOP) {
                    state->running = false;
                } else if (p[2] == UBX_CFG_RST_START) {
                    state->running = true;
                }
                state->resets++;
                mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_COMMAND, p[2], 0, 0);
            }
            break;
        case UBX_CFG_RATE:
            if (state->len == 0) {
                send_cfg_rate(state);
            } else if (state->len == UBX_CFG_RATE_LEN) {
                uint16_t rate = get_u16(&p[0]);

                if (rate < NEO_M8N_RATE_MS_MIN) {
                    send_ack(state, false, state->cls, state->id);
                    break;
                }
                state->rate_ms = rate;
                state->nav_rate = get_u16(&p[2]);
                state->time_ref = get_u16(&p[4]);
                send_ack(state, true, state->cls, state->id);
            } else {
                send_ack(state, false, state->cls, state->id);
            }
            break;
        case UBX_CFG_NAV5:
            if (state->len == 0) {
                send_cfg_nav5(state);
            } else if (state->len == UBX_CFG_NAV5_LEN) {
                if ((get_u16(&p[0]) & UBX_CFG_NAV5_APPLY_DYN) != 0) {
                    state->dyn_model = p[2];
                }
                send_ack(state, true, state->cls, state->id);
            } else {
                send_ack(state, false, state->cls, state->id);
            }
            break;
        default:
            send_ack(state, false, state->cls, state->id);
            break;
    }
}

static void handle_frame(struct neo_m8n_state *state) {
    if (state->oversize) {
        send_ack(state, false, state->cls, state->id);
        return;
    }
    switch (state->cls) {
        case UBX_CLASS_CFG:
            handle_cfg(state);
            break;
        case UBX_CLASS_MON:
            if (state->id == UBX_MON_VER) {
                send_mon_ver(state);
            } else if (state->id == UBX_MON_GNSS) {
                send_mon_gnss(state);
            } else {
                send_ack(state, false, state->cls, state->id);
            }
            break;
        default:
            send_ack(state, false, state->cls, state->id);
            break;
    }
}

static void parse_byte(struct neo_m8n_state *state, uint8_t byte) {
    switch (state->parse) {
        case PARSE_SYNC_1:
            if (byte == UBX_SYNC_1) {
                state->parse = PARSE_SYNC_2;
            }
            break;
        case PARSE_SYNC_2:
            state->parse = byte == UBX_SYNC_2 ? PARSE_CLASS : (byte == UBX_SYNC_1 ? PARSE_SYNC_2 : PARSE_SYNC_1);
            break;
        case PARSE_CLASS:
            state->cls = byte;
            state->ck_a = byte;
            state->ck_b = byte;
            state->parse = PARSE_ID;
            break;
        case PARSE_ID:
            state->id = byte;
            state->ck_a = (uint8_t)(state->ck_a + byte);
            state->ck_b = (uint8_t)(state->ck_b + state->ck_a);
            state->parse = PARSE_LEN_LO;
            break;
        case PARSE_LEN_LO:
            state->len = byte;
            state->ck_a = (uint8_t)(state->ck_a + byte);
            state->ck_b = (uint8_t)(state->ck_b + state->ck_a);
            state->parse = PARSE_LEN_HI;
            break;
        case PARSE_LEN_HI:
            state->len |= (uint16_t)(byte << 8);
            state->ck_a = (uint8_t)(state->ck_a + byte);
            state->ck_b = (uint8_t)(state->ck_b + state->ck_a);
            state->got = 0;
            state->oversize = state->len > UBX_PAYLOAD_MAX;
            state->parse = state->len == 0 ? PARSE_CK_A : PARSE_PAYLOAD;
            break;
        case PARSE_PAYLOAD:
            if (!state->oversize) {
                state->payload[state->got] = byte;
            }
            state->got++;
            state->ck_a = (uint8_t)(state->ck_a + byte);
            state->ck_b = (uint8_t)(state->ck_b + state->ck_a);
            if (state->got == state->len) {
                state->parse = PARSE_CK_A;
            }
            break;
        case PARSE_CK_A:
            state->parse = byte == state->ck_a ? PARSE_CK_B : PARSE_SYNC_1;
            break;
        case PARSE_CK_B:
            state->parse = PARSE_SYNC_1;
            if (byte == state->ck_b) {
                handle_frame(state);
            }
            break;
    }
}

static void on_rx(struct mclw_uart *uart, const uint8_t *bytes, size_t len, void *ctx) {
    struct neo_m8n_state *state = ctx;

    (void)uart;
    for (size_t i = 0; i < len; i++) {
        parse_byte(state, bytes[i]);
    }
}

static void power_on(struct neo_m8n_state *state) {
    state->latitude = 0;
    state->longitude = 0;
    state->altitude = 0;
    state->speed = 0;
    state->heading = 0;
    state->satellites = 0;
    state->fix = UBX_NAV_PVT_FIX_NONE;
    state->pdop = NEO_M8N_DEFAULT_PDOP;
    memset(state->time, 0, sizeof(state->time));
    state->time[0] = 2015;
    state->time[1] = 1;
    state->time[2] = 1;
    state->rate_ms = NEO_M8N_DEFAULT_RATE_MS;
    state->nav_rate = NEO_M8N_DEFAULT_NAV_RATE;
    state->time_ref = NEO_M8N_DEFAULT_TIME_REF;
    state->nav_pvt_rate = 0;
    state->dyn_model = NEO_M8N_DEFAULT_DYN_MODEL;
    state->running = true;
    state->resets = 0;
    state->fixes = 0;
    state->next_fix_us = 0;
    state->parse = PARSE_SYNC_1;
}

static int neo_m8n_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct neo_m8n_state *state = inst->state;

    state->uart = bindings[BINDING_UART].uart;
    power_on(state);
    state->baud = neo_m8n_bindings[BINDING_UART].uart.default_baud;
    mclw_hal_uart_set_rx(state->uart, on_rx, state);
    return 0;
}

static void neo_m8n_deinit(struct mclw_instance *inst) {
    struct neo_m8n_state *state = inst->state;

    mclw_hal_uart_set_rx(state->uart, NULL, NULL);
    state->uart = NULL;
}

static int neo_m8n_reset(struct mclw_instance *inst, bool factory) {
    struct neo_m8n_state *state = inst->state;

    (void)factory;
    power_on(state);
    apply_baud(state, NEO_M8N_DEFAULT_BAUD);
    return 0;
}

static int neo_m8n_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct neo_m8n_state *state = inst->state;

    switch (property) {
        case PROP_LATITUDE:
            out->v[0] = state->latitude;
            return 0;
        case PROP_LONGITUDE:
            out->v[0] = state->longitude;
            return 0;
        case PROP_ALTITUDE:
            out->v[0] = state->altitude;
            return 0;
        case PROP_SPEED:
            out->v[0] = state->speed;
            return 0;
        case PROP_HEADING:
            out->v[0] = state->heading;
            return 0;
        case PROP_SATELLITES:
            out->v[0] = state->satellites;
            return 0;
        case PROP_FIX:
            out->v[0] = state->fix;
            return 0;
        case PROP_PDOP:
            out->v[0] = state->pdop;
            return 0;
        case PROP_TIME:
            for (size_t i = 0; i < 6; i++) {
                out->v[i] = state->time[i];
            }
            return 0;
        case PROP_BAUD:
            out->v[0] = state->baud;
            return 0;
        case PROP_RATE_MS:
            out->v[0] = state->rate_ms;
            return 0;
        case PROP_NAV_PVT_RATE:
            out->v[0] = state->nav_pvt_rate;
            return 0;
        case PROP_DYN_MODEL:
            out->v[0] = state->dyn_model;
            return 0;
        case PROP_RUNNING:
            out->v[0] = state->running ? 1 : 0;
            return 0;
        case PROP_RESETS:
            out->v[0] = state->resets;
            return 0;
        case PROP_FIXES:
            out->v[0] = state->fixes;
            return 0;
        default:
            return -EINVAL;
    }
}

static int neo_m8n_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct neo_m8n_state *state = inst->state;

    switch (property) {
        case PROP_LATITUDE:
            state->latitude = (int32_t)in->v[0];
            return 0;
        case PROP_LONGITUDE:
            state->longitude = (int32_t)in->v[0];
            return 0;
        case PROP_ALTITUDE:
            state->altitude = (int32_t)in->v[0];
            return 0;
        case PROP_SPEED:
            state->speed = (uint32_t)in->v[0];
            return 0;
        case PROP_HEADING:
            state->heading = (int32_t)in->v[0];
            return 0;
        case PROP_SATELLITES:
            state->satellites = (uint8_t)in->v[0];
            return 0;
        case PROP_FIX:
            state->fix = (uint8_t)in->v[0];
            return 0;
        case PROP_PDOP:
            state->pdop = (uint16_t)in->v[0];
            return 0;
        case PROP_TIME:
            for (size_t i = 0; i < 6; i++) {
                state->time[i] = (int32_t)in->v[i];
            }
            return 0;
        case PROP_BAUD:
            apply_baud(state, (uint32_t)in->v[0]);
            return state->baud == (uint32_t)in->v[0] ? 0 : -EIO;
        default:
            return -EINVAL;
    }
}

// One NAV-PVT per fix interval while the receiver runs and the DUT asked
// for the message; a jump of several intervals still produces one frame.
static void neo_m8n_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct neo_m8n_state *state = inst->state;

    if (now_us < state->next_fix_us) {
        return;
    }
    state->next_fix_us = now_us + (uint64_t)state->rate_ms * 1000u;
    if (state->running && state->nav_pvt_rate != 0) {
        send_nav_pvt(state);
    }
}

static const struct mclw_model_ops neo_m8n_ops = {
    .init = neo_m8n_init,
    .deinit = neo_m8n_deinit,
    .reset = neo_m8n_reset,
    .get = neo_m8n_get,
    .set = neo_m8n_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = neo_m8n_time_advanced,
    .usb_rx = NULL,
};

const struct mclw_model mclw_model_neo_m8n = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "neo_m8n",
    .family = "neo_m8n",
    .config = NULL,
    .properties = neo_m8n_properties,
    .property_count = sizeof(neo_m8n_properties) / sizeof(neo_m8n_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = neo_m8n_bindings,
    .binding_count = sizeof(neo_m8n_bindings) / sizeof(neo_m8n_bindings[0]),
    .state_size = sizeof(struct neo_m8n_state),
    .ops = &neo_m8n_ops,
};
