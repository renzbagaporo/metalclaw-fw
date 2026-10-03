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
    PROP_HDOP,
    PROP_VDOP,
    PROP_PROTOCOLS,
    PROP_NMEA_RATES,
    PROP_NMEA_VERSION,
    PROP_SENTENCES,
    PROP_DROPPED,
    PROP_GNSS,
    PROP_BACKUP,
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

// The GNSS the synthetic satellites belong to, each with its own GSA and
// GSV sentences.
enum {
    SYS_GPS,
    SYS_GLONASS,
    SYS_COUNT,
};

#define GSV_PAGES_MAX ((NEO_M8N_SATELLITES_MAX + NMEA_GSV_SVS_PER_PAGE - 1) / NMEA_GSV_SVS_PER_PAGE)

// One epoch's NMEA, in the order the part sends it: every slot that exists
// for this epoch becomes one sentence.
enum {
    SLOT_RMC,
    SLOT_VTG,
    SLOT_GGA,
    SLOT_GSA,
    SLOT_GSV = SLOT_GSA + SYS_COUNT,
    SLOT_GLL = SLOT_GSV + SYS_COUNT * GSV_PAGES_MAX,
    SLOT_END,
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
    uint16_t hdop;
    uint16_t vdop;
    // UTC in ms since 1970. A set waits in utc_set_ms for the next fix,
    // which anchors it to that epoch's simulation time.
    int64_t utc_set_ms;
    int64_t utc_anchor_ms;
    uint64_t utc_anchor_us;
    bool utc_pending;
    int64_t epoch_utc_ms;
    uint32_t baud;
    uint16_t rate_ms;
    uint16_t nav_rate;
    uint16_t time_ref;
    uint16_t in_proto;
    uint16_t out_proto;
    uint8_t nav_pvt_rate;
    uint8_t nmea_rate[NMEA_STANDARD_COUNT];
    uint8_t nmea_version;
    uint8_t gnss;
    uint8_t dyn_model;
    bool running;
    // Software backup after RXM-PMREQ: silent until backup_until_us (0 for
    // never) or, if allowed, until anything arrives on the port.
    bool backup;
    bool backup_rx_wake;
    uint64_t backup_until_us;
    uint32_t resets;
    uint32_t fixes;
    uint32_t sentences;
    uint32_t dropped;
    uint32_t epochs;
    uint64_t next_fix_us;
    // NMEA due this epoch (bit per message id) and the next slot to send.
    uint8_t nmea_due;
    uint8_t nmea_slot;
    // Bytes handed to the engine, against its bytes_tx, for the queue depth.
    uint32_t tx_queued;
    uint32_t tx_base;
    enum parse_state parse;
    uint8_t cls;
    uint8_t id;
    uint16_t len;
    uint16_t got;
    uint8_t ck_a;
    uint8_t ck_b;
    bool oversize;
    uint8_t payload[UBX_PAYLOAD_MAX];
    // An NMEA input line from '$'; 0 when none is being collected.
    uint8_t line_len;
    bool line_overflow;
    char line[NMEA_LINE_MAX + 1];
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
    {
        .name = "hdop",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "0.01",
        .min = 0,
        .max = UINT16_MAX,
    },
    {
        .name = "vdop",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "0.01",
        .min = 0,
        .max = UINT16_MAX,
    },
    {
        .name = "protocols",
        .type = MCLW_TYPE_U32,
        .count = 2,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT16_MAX,
    },
    {
        .name = "nmea_rates",
        .type = MCLW_TYPE_U32,
        .count = NMEA_STANDARD_COUNT,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 255,
    },
    {
        .name = "nmea_version",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 255,
    },
    {
        .name = "sentences",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "dropped",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "gnss",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 255,
    },
    {
        .name = "backup",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
};

// MON-VER's extension strings, as a NEO-M8N running ROM 3.01 reports them.
// Meshtastic reads the protocol version from PROTVER.
static const char neo_m8n_extensions[][UBX_MON_VER_EXTENSION_LEN] = {
    "FWVER=SPG 3.01",
    "PROTVER=18.00",
    "GPS;GLO;GAL;BDS",
    "SBAS;IMES;QZSS",
    "GNSS OTP=GPS;GLO",
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

static int64_t floor_div(int64_t a, int64_t b) {
    return a / b - ((a % b != 0 && (a < 0) != (b < 0)) ? 1 : 0);
}

// Proleptic Gregorian calendar, days relative to 1970-01-01 (H. Hinnant,
// "chrono-compatible low-level date algorithms").
static int64_t days_from_civil(int64_t year, int64_t month, int64_t day) {
    int64_t m0 = month - 1;
    int64_t y = year + floor_div(m0, 12);
    int64_t m = m0 - floor_div(m0, 12) * 12 + 1;

    y -= m <= 2 ? 1 : 0;
    int64_t era = floor_div(y, 400);
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Fields: year, month, day, hour, minute, second, millisecond.
static void civil_from_ms(int64_t utc_ms, int32_t fields[7]) {
    int64_t days = floor_div(utc_ms, 86400000);
    int64_t ms = utc_ms - days * 86400000;
    int64_t z = days + 719468;
    int64_t era = floor_div(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t month = mp < 10 ? mp + 3 : mp - 9;

    fields[0] = (int32_t)(yoe + era * 400 + (month <= 2 ? 1 : 0));
    fields[1] = (int32_t)month;
    fields[2] = (int32_t)(doy - (153 * mp + 2) / 5 + 1);
    fields[3] = (int32_t)(ms / 3600000);
    fields[4] = (int32_t)(ms / 60000 % 60);
    fields[5] = (int32_t)(ms / 1000 % 60);
    fields[6] = (int32_t)(ms % 1000);
}

static int64_t ms_from_civil(const int64_t fields[6]) {
    int64_t days = days_from_civil(fields[0], fields[1], fields[2]);

    return (((days * 24 + fields[3]) * 60 + fields[4]) * 60 + fields[5]) * 1000;
}

// The UTC of an epoch at simulation time t_us. A pending set takes effect
// here: the first fix after it carries exactly the time set.
static int64_t utc_at_epoch(struct neo_m8n_state *state, uint64_t t_us) {
    if (state->utc_pending) {
        state->utc_anchor_ms = state->utc_set_ms;
        state->utc_anchor_us = t_us;
        state->utc_pending = false;
    }
    return state->utc_anchor_ms + (int64_t)((t_us - state->utc_anchor_us) / 1000u);
}

static bool uart_out(struct neo_m8n_state *state, const void *bytes, size_t len) {
    size_t sent = len == 0 ? 0 : mclw_hal_uart_send(state->uart, bytes, len);

    state->tx_queued += (uint32_t)sent;
    return sent == len;
}

// What the engine has not put on the wire yet. Counting by its bytes_tx
// errs high (an engine may count a run only once it completes), never low.
static uint32_t tx_outstanding(const struct neo_m8n_state *state) {
    struct mclw_uart_stats stats;

    mclw_hal_uart_stats(state->uart, &stats);
    return state->tx_queued - (stats.bytes_tx - state->tx_base);
}

// Queues one UBX frame: sync, class, id, little-endian length, payload and
// the 8-bit Fletcher checksum over class through payload.
static void send_frame(struct neo_m8n_state *state, uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
    uint8_t head[UBX_HEADER_BYTES] = { UBX_SYNC_1, UBX_SYNC_2, cls, id, (uint8_t)len, (uint8_t)(len >> 8) };
    uint8_t ck[UBX_TRAILER_BYTES] = { 0, 0 };
    uint16_t total = (uint16_t)(UBX_HEADER_BYTES + len + UBX_TRAILER_BYTES);

    if ((state->out_proto & UBX_PROTO_UBX) == 0) {
        return;
    }
    for (size_t i = 2; i < UBX_HEADER_BYTES; i++) {
        ck[0] = (uint8_t)(ck[0] + head[i]);
        ck[1] = (uint8_t)(ck[1] + ck[0]);
    }
    for (size_t i = 0; i < len; i++) {
        ck[0] = (uint8_t)(ck[0] + payload[i]);
        ck[1] = (uint8_t)(ck[1] + ck[0]);
    }
    if (uart_out(state, head, sizeof(head)) && uart_out(state, payload, len) && uart_out(state, ck, sizeof(ck))) {
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_UART, ((uint32_t)cls << 8) | id, total, 0);
    } else {
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_ERROR, ((uint32_t)cls << 8) | id, total, -ENOSPC);
    }
}

static void send_ack(struct neo_m8n_state *state, bool ack, uint8_t cls, uint8_t id) {
    uint8_t payload[2] = { cls, id };

    send_frame(state, UBX_CLASS_ACK, ack ? UBX_ACK_ACK : UBX_ACK_NAK, payload, sizeof(payload));
}

static void send_mon_ver(struct neo_m8n_state *state) {
    uint8_t payload[UBX_MON_VER_LEN + sizeof(neo_m8n_extensions)];

    memset(payload, 0, sizeof(payload));
    memcpy(payload, NEO_M8N_SW_VERSION, sizeof(NEO_M8N_SW_VERSION));
    memcpy(&payload[30], NEO_M8N_HW_VERSION, sizeof(NEO_M8N_HW_VERSION));
    memcpy(&payload[UBX_MON_VER_LEN], neo_m8n_extensions, sizeof(neo_m8n_extensions));
    send_frame(state, UBX_CLASS_MON, UBX_MON_VER, payload, sizeof(payload));
}

static void send_mon_gnss(struct neo_m8n_state *state) {
    uint8_t payload[UBX_MON_GNSS_LEN];

    memset(payload, 0, sizeof(payload));
    payload[1] = UBX_GNSS_GPS | UBX_GNSS_GLONASS | UBX_GNSS_BEIDOU | UBX_GNSS_GALILEO;
    payload[2] = state->gnss;
    payload[3] = state->gnss;
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
    put_u32(&payload[4], UBX_CFG_PRT_MODE_8N1);
    put_u32(&payload[8], state->baud);
    put_u16(&payload[12], state->in_proto);
    put_u16(&payload[14], state->out_proto);
    send_frame(state, UBX_CLASS_CFG, UBX_CFG_PRT, payload, sizeof(payload));
}

static uint8_t *msg_rate(struct neo_m8n_state *state, uint8_t cls, uint8_t id) {
    if (cls == UBX_CLASS_NAV && id == UBX_NAV_PVT) {
        return &state->nav_pvt_rate;
    }
    if (cls == UBX_CLASS_NMEA && id < NMEA_STANDARD_COUNT) {
        return &state->nmea_rate[id];
    }
    return NULL;
}

// The 8-byte form: the rate on each port, DDC, UART1, UART2, USB, SPI.
// Only UART1 exists here; the others read 0.
static void send_cfg_msg(struct neo_m8n_state *state, uint8_t cls, uint8_t id) {
    uint8_t payload[UBX_CFG_MSG_PORTS_LEN];
    const uint8_t *rate = msg_rate(state, cls, id);

    memset(payload, 0, sizeof(payload));
    payload[0] = cls;
    payload[1] = id;
    payload[2 + UBX_CFG_MSG_PORT_UART1] = rate != NULL ? *rate : 0;
    send_frame(state, UBX_CLASS_CFG, UBX_CFG_MSG, payload, sizeof(payload));
}

static void send_nav_pvt(struct neo_m8n_state *state) {
    uint8_t p[UBX_NAV_PVT_LEN];
    uint8_t valid = 0;
    uint8_t flags = 0;
    int32_t utc[7];

    memset(p, 0, sizeof(p));
    if (state->fix != UBX_NAV_PVT_FIX_NONE) {
        valid = UBX_NAV_PVT_VALID_DATE | UBX_NAV_PVT_VALID_TIME | UBX_NAV_PVT_VALID_FULLY_RESOLVED;
        flags = UBX_NAV_PVT_FLAGS_GNSS_FIX_OK;
    }
    civil_from_ms(state->epoch_utc_ms, utc);
    put_u32(&p[0], (uint32_t)(state->fixes * (uint32_t)state->rate_ms));
    put_u16(&p[4], (uint16_t)utc[0]);
    p[6] = (uint8_t)utc[1];
    p[7] = (uint8_t)utc[2];
    p[8] = (uint8_t)utc[3];
    p[9] = (uint8_t)utc[4];
    p[10] = (uint8_t)utc[5];
    p[11] = valid;
    put_u32(&p[16], (uint32_t)(utc[6] * 1000000));
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

struct nmea_out {
    char s[NMEA_SENTENCE_MAX];
    size_t len;
    bool overflow;
};

static void out_char(struct nmea_out *o, char c) {
    if (o->len < sizeof(o->s)) {
        o->s[o->len++] = c;
    } else {
        o->overflow = true;
    }
}

static void out_str(struct nmea_out *o, const char *str) {
    while (*str != '\0') {
        out_char(o, *str++);
    }
}

static void out_uint(struct nmea_out *o, uint64_t v, unsigned width) {
    char digits[20];
    unsigned n = 0;

    do {
        digits[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0 && n < sizeof(digits));
    while (n < width && n < sizeof(digits)) {
        digits[n++] = '0';
    }
    while (n > 0) {
        out_char(o, digits[--n]);
    }
}

// v in units of 10^-decimals.
static void out_fixed(struct nmea_out *o, int64_t v, unsigned decimals) {
    uint64_t scale = 1;
    uint64_t mag = v < 0 ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;

    for (unsigned i = 0; i < decimals; i++) {
        scale *= 10u;
    }
    if (v < 0) {
        out_char(o, '-');
    }
    out_uint(o, mag / scale, 1);
    if (decimals != 0) {
        out_char(o, '.');
        out_uint(o, mag % scale, decimals);
    }
}

// Closes the sentence: '*', the XOR of everything between '$' and '*' in
// hex, CR LF. 0 if it did not fit.
static size_t out_finish(struct nmea_out *o) {
    static const char hex[] = "0123456789ABCDEF";
    uint8_t x = 0;

    for (size_t i = 1; i < o->len; i++) {
        x ^= (uint8_t)o->s[i];
    }
    out_char(o, '*');
    out_char(o, hex[x >> 4]);
    out_char(o, hex[x & 0x0f]);
    out_char(o, '\r');
    out_char(o, '\n');
    return o->overflow ? 0 : o->len;
}

static bool fix_has_position(uint8_t fix) {
    return fix != UBX_NAV_PVT_FIX_NONE && fix != UBX_NAV_PVT_FIX_TIME_ONLY;
}

// The position fix flags table of the receiver description: GGA quality,
// GSA navMode and posMode by fix type.
static char gga_quality(uint8_t fix) {
    if (fix == UBX_NAV_PVT_FIX_DEAD_RECKONING) {
        return '6';
    }
    return fix_has_position(fix) ? '1' : '0';
}

static char pos_mode(uint8_t fix) {
    if (fix == UBX_NAV_PVT_FIX_DEAD_RECKONING) {
        return 'E';
    }
    return fix_has_position(fix) ? 'A' : 'N';
}

static char nav_mode(uint8_t fix) {
    if (fix >= UBX_NAV_PVT_FIX_3D && fix_has_position(fix)) {
        return '3';
    }
    return fix_has_position(fix) ? '2' : '1';
}

// GN when more than one GNSS is enabled, else that system's own talker.
static const char *main_talker(const struct neo_m8n_state *state) {
    static const struct {
        uint8_t mask;
        const char *talker;
    } single[] = {
        { UBX_GNSS_GPS, "GP" },
        { UBX_GNSS_GLONASS, "GL" },
        { UBX_GNSS_BEIDOU, "GB" },
        { UBX_GNSS_GALILEO, "GA" },
    };
    uint8_t mask = state->gnss & (UBX_GNSS_GPS | UBX_GNSS_GLONASS | UBX_GNSS_BEIDOU | UBX_GNSS_GALILEO);

    for (size_t i = 0; i < sizeof(single) / sizeof(single[0]); i++) {
        if (mask == single[i].mask) {
            return single[i].talker;
        }
    }
    return mask == 0 ? "GP" : "GN";
}

static bool sys_present(const struct neo_m8n_state *state, unsigned sys) {
    bool glonass = (state->gnss & UBX_GNSS_GLONASS) != 0;

    return sys == SYS_GLONASS ? glonass : ((state->gnss & UBX_GNSS_GPS) != 0 || !glonass);
}

// The satellites split between GPS and GLONASS when both are enabled.
static unsigned sys_satellites(const struct neo_m8n_state *state, unsigned sys) {
    if (!sys_present(state, sys)) {
        return 0;
    }
    if (sys_present(state, SYS_GPS) && sys_present(state, SYS_GLONASS)) {
        return sys == SYS_GPS ? (state->satellites + 1u) / 2u : state->satellites / 2u;
    }
    return state->satellites;
}

static unsigned gsv_pages(const struct neo_m8n_state *state, unsigned sys) {
    unsigned n = sys_satellites(state, sys);

    return n == 0 ? 1 : (n + NMEA_GSV_SVS_PER_PAGE - 1) / NMEA_GSV_SVS_PER_PAGE;
}

static unsigned svid(unsigned sys, unsigned k) {
    return sys == SYS_GPS ? 1u + k : NMEA_GLONASS_SVID_BASE + k;
}

static bool nmea_41(const struct neo_m8n_state *state) {
    return state->nmea_version == NMEA_VERSION_41;
}

static void out_head(struct nmea_out *o, const char *talker, const char *type) {
    out_char(o, '$');
    out_str(o, talker);
    out_str(o, type);
}

static void out_time(struct nmea_out *o, const int32_t utc[7]) {
    out_uint(o, (uint64_t)utc[3], 2);
    out_uint(o, (uint64_t)utc[4], 2);
    out_uint(o, (uint64_t)utc[5], 2);
    out_char(o, '.');
    out_uint(o, (uint64_t)(utc[6] / 10), 2);
}

// ddmm.mmmmm,N or dddmm.mmmmm,E: degrees and minutes to 1e-5 minute.
static void out_coordinate(struct nmea_out *o, int32_t v, unsigned degree_digits, char positive, char negative) {
    uint32_t mag = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    uint32_t degrees = mag / 10000000u;
    uint32_t minutes_e5 = ((mag % 10000000u) * 60u + 50u) / 100u;

    if (minutes_e5 >= 6000000u) {
        degrees++;
        minutes_e5 -= 6000000u;
    }
    out_uint(o, degrees, degree_digits);
    out_uint(o, minutes_e5 / 100000u, 2);
    out_char(o, '.');
    out_uint(o, minutes_e5 % 100000u, 5);
    out_char(o, ',');
    out_char(o, v < 0 ? negative : positive);
}

static void out_position(struct nmea_out *o, const struct neo_m8n_state *state) {
    out_coordinate(o, state->latitude, 2, 'N', 'S');
    out_char(o, ',');
    out_coordinate(o, state->longitude, 3, 'E', 'W');
}

static void out_dop(struct nmea_out *o, uint16_t dop) {
    out_fixed(o, dop > NMEA_DOP_UNKNOWN ? NMEA_DOP_UNKNOWN : dop, 2);
}

static uint64_t speed_milli_knots(const struct neo_m8n_state *state) {
    return ((uint64_t)state->speed * 3600u + 926u) / 1852u;
}

static void out_course(struct nmea_out *o, const struct neo_m8n_state *state) {
    uint32_t centi = ((uint32_t)state->heading + 500u) / 1000u;

    out_fixed(o, centi >= 36000u ? centi - 36000u : centi, 2);
}

static void build_rmc(const struct neo_m8n_state *state, const int32_t utc[7], struct nmea_out *o) {
    bool position = fix_has_position(state->fix);
    bool has_time = state->fix != UBX_NAV_PVT_FIX_NONE;

    out_head(o, main_talker(state), "RMC,");
    if (has_time) {
        out_time(o, utc);
    }
    out_char(o, ',');
    out_char(o, position ? 'A' : 'V');
    out_char(o, ',');
    if (position) {
        out_position(o, state);
        out_char(o, ',');
        out_fixed(o, (int64_t)speed_milli_knots(state), 3);
        out_char(o, ',');
        out_course(o, state);
    } else {
        out_str(o, ",,,,,");
    }
    out_char(o, ',');
    if (has_time) {
        out_uint(o, (uint64_t)utc[2], 2);
        out_uint(o, (uint64_t)utc[1], 2);
        out_uint(o, (uint64_t)(utc[0] % 100), 2);
    }
    out_str(o, ",,,");
    out_char(o, pos_mode(state->fix));
    if (nmea_41(state)) {
        out_str(o, ",V");
    }
}

static void build_vtg(const struct neo_m8n_state *state, struct nmea_out *o) {
    out_head(o, main_talker(state), "VTG,");
    if (fix_has_position(state->fix)) {
        out_course(o, state);
        out_str(o, ",T,,M,");
        out_fixed(o, (int64_t)speed_milli_knots(state), 3);
        out_str(o, ",N,");
        out_fixed(o, (int64_t)(((uint64_t)state->speed * 36u + 5u) / 10u), 3);
        out_str(o, ",K,");
    } else {
        out_str(o, ",,,,,,,,");
    }
    out_char(o, pos_mode(state->fix));
}

static void build_gga(const struct neo_m8n_state *state, const int32_t utc[7], struct nmea_out *o) {
    bool position = fix_has_position(state->fix);

    out_head(o, main_talker(state), "GGA,");
    if (state->fix != UBX_NAV_PVT_FIX_NONE) {
        out_time(o, utc);
    }
    out_char(o, ',');
    if (position) {
        out_position(o, state);
    } else {
        out_str(o, ",,,");
    }
    out_char(o, ',');
    out_char(o, gga_quality(state->fix));
    out_char(o, ',');
    out_uint(o, position ? state->satellites : 0, 2);
    out_char(o, ',');
    out_dop(o, position ? state->hdop : NMEA_DOP_UNKNOWN);
    out_char(o, ',');
    if (position) {
        // Altitude above mean sea level to 0.1 m; NAV-PVT sends the same
        // value as height, so the geoid separation is 0.
        int64_t alt = state->altitude;
        out_fixed(o, (alt + (alt < 0 ? -50 : 50)) / 100, 1);
        out_str(o, ",M,0.0,M,,");
    } else {
        out_str(o, ",,,,,");
    }
}

static void build_gsa(const struct neo_m8n_state *state, unsigned sys, struct nmea_out *o) {
    bool position = fix_has_position(state->fix);
    unsigned used = position ? sys_satellites(state, sys) : 0;

    out_head(o, main_talker(state), "GSA,A,");
    out_char(o, nav_mode(state->fix));
    out_char(o, ',');
    for (unsigned k = 0; k < NMEA_GSA_SVS; k++) {
        if (k < used) {
            out_uint(o, svid(sys, k), 2);
        }
        out_char(o, ',');
    }
    out_dop(o, position ? state->pdop : NMEA_DOP_UNKNOWN);
    out_char(o, ',');
    out_dop(o, position ? state->hdop : NMEA_DOP_UNKNOWN);
    out_char(o, ',');
    out_dop(o, position ? state->vdop : NMEA_DOP_UNKNOWN);
    if (nmea_41(state)) {
        out_str(o, sys == SYS_GPS ? ",1" : ",2");
    }
}

// Satellites in view, synthetic: GPS PRNs from 1, GLONASS slots from 65,
// with an elevation, azimuth and C/N0 that differ per satellite.
static void build_gsv(const struct neo_m8n_state *state, unsigned sys, unsigned page, struct nmea_out *o) {
    unsigned n = sys_satellites(state, sys);
    unsigned first = page * NMEA_GSV_SVS_PER_PAGE;

    out_head(o, sys == SYS_GPS ? "GP" : "GL", "GSV,");
    out_uint(o, gsv_pages(state, sys), 1);
    out_char(o, ',');
    out_uint(o, page + 1u, 1);
    out_char(o, ',');
    out_uint(o, n, 2);
    for (unsigned k = first; k < n && k < first + NMEA_GSV_SVS_PER_PAGE; k++) {
        out_char(o, ',');
        out_uint(o, svid(sys, k), 2);
        out_char(o, ',');
        out_uint(o, 10u + (k * 23u + sys * 11u) % 80u, 2);
        out_char(o, ',');
        out_uint(o, (k * 77u + sys * 41u) % 360u, 3);
        out_char(o, ',');
        out_uint(o, 25u + (k * 7u + sys * 3u) % 25u, 2);
    }
    if (nmea_41(state)) {
        out_str(o, ",1");
    }
}

static void build_gll(const struct neo_m8n_state *state, const int32_t utc[7], struct nmea_out *o) {
    bool position = fix_has_position(state->fix);

    out_head(o, main_talker(state), "GLL,");
    if (position) {
        out_position(o, state);
    } else {
        out_str(o, ",,,");
    }
    out_char(o, ',');
    if (state->fix != UBX_NAV_PVT_FIX_NONE) {
        out_time(o, utc);
    }
    out_char(o, ',');
    out_char(o, position ? 'A' : 'V');
    out_char(o, ',');
    out_char(o, pos_mode(state->fix));
}

static bool nmea_due(const struct neo_m8n_state *state, unsigned msg) {
    return (state->nmea_due & (1u << msg)) != 0;
}

static bool slot_exists(const struct neo_m8n_state *state, unsigned slot) {
    if (slot == SLOT_RMC) {
        return nmea_due(state, NMEA_RMC);
    }
    if (slot == SLOT_VTG) {
        return nmea_due(state, NMEA_VTG);
    }
    if (slot == SLOT_GGA) {
        return nmea_due(state, NMEA_GGA);
    }
    if (slot < SLOT_GSV) {
        return nmea_due(state, NMEA_GSA) && sys_present(state, slot - SLOT_GSA);
    }
    if (slot < SLOT_GLL) {
        unsigned sys = (slot - SLOT_GSV) / GSV_PAGES_MAX;

        return nmea_due(state, NMEA_GSV) && sys_present(state, sys) &&
            (slot - SLOT_GSV) % GSV_PAGES_MAX < gsv_pages(state, sys);
    }
    return slot == SLOT_GLL && nmea_due(state, NMEA_GLL);
}

static uint8_t slot_message(unsigned slot) {
    static const uint8_t first[] = { NMEA_RMC, NMEA_VTG, NMEA_GGA };

    if (slot < SLOT_GSA) {
        return first[slot];
    }
    if (slot < SLOT_GSV) {
        return NMEA_GSA;
    }
    return slot < SLOT_GLL ? NMEA_GSV : NMEA_GLL;
}

static size_t build_slot(const struct neo_m8n_state *state, unsigned slot, struct nmea_out *o) {
    int32_t utc[7];

    civil_from_ms(state->epoch_utc_ms, utc);
    o->len = 0;
    o->overflow = false;
    if (slot == SLOT_RMC) {
        build_rmc(state, utc, o);
    } else if (slot == SLOT_VTG) {
        build_vtg(state, o);
    } else if (slot == SLOT_GGA) {
        build_gga(state, utc, o);
    } else if (slot < SLOT_GSV) {
        build_gsa(state, slot - SLOT_GSA, o);
    } else if (slot < SLOT_GLL) {
        build_gsv(state, (slot - SLOT_GSV) / GSV_PAGES_MAX, (slot - SLOT_GSV) % GSV_PAGES_MAX, o);
    } else {
        build_gll(state, utc, o);
    }
    return out_finish(o);
}

static void nmea_drop_pending(struct neo_m8n_state *state) {
    for (unsigned slot = state->nmea_slot; slot < SLOT_END; slot++) {
        if (slot_exists(state, slot)) {
            state->dropped++;
        }
    }
    state->nmea_slot = SLOT_END;
}

// Sends the epoch's next sentences while the engine's queue holds less
// than the budget; the rest wait for a later call.
static void nmea_pump(struct neo_m8n_state *state) {
    while (state->nmea_slot < SLOT_END) {
        unsigned slot = state->nmea_slot;
        struct nmea_out o;
        size_t len;

        if (!slot_exists(state, slot)) {
            state->nmea_slot++;
            continue;
        }
        if (tx_outstanding(state) > NEO_M8N_TX_BUDGET - NMEA_SENTENCE_MAX) {
            return;
        }
        len = build_slot(state, slot, &o);
        state->nmea_slot++;
        if (len != 0 && uart_out(state, o.s, len)) {
            state->sentences++;
            mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_UART,
                ((uint32_t)UBX_CLASS_NMEA << 8) | slot_message(slot), (uint16_t)len, 0);
        } else {
            state->dropped++;
            mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_ERROR,
                ((uint32_t)UBX_CLASS_NMEA << 8) | slot_message(slot), (uint16_t)len, -ENOSPC);
        }
    }
}

static void nmea_epoch(struct neo_m8n_state *state) {
    uint8_t due = 0;

    for (unsigned i = 0; i < NMEA_STANDARD_COUNT; i++) {
        if (state->nmea_rate[i] != 0 && state->epochs % state->nmea_rate[i] == 0) {
            due |= (uint8_t)(1u << i);
        }
    }
    nmea_drop_pending(state);
    state->nmea_due = due;
    state->nmea_slot = 0;
}

static void set_out_proto(struct neo_m8n_state *state, uint16_t mask) {
    state->out_proto = mask;
    if ((mask & UBX_PROTO_NMEA) == 0) {
        nmea_drop_pending(state);
    }
}

static void apply_baud(struct neo_m8n_state *state, uint32_t baud) {
    if (mclw_hal_uart_set_baud(state->uart, baud) == 0) {
        state->baud = baud;
    }
}

static bool nmea_version_valid(uint8_t version) {
    return version == NMEA_VERSION_21 || version == NMEA_VERSION_23 || version == NMEA_VERSION_40 ||
        version == NMEA_VERSION_41;
}

// CFG messages the part takes that change nothing the model shows: the set
// is acknowledged, the poll is not modelled.
static bool cfg_accepted(uint8_t id, uint16_t len) {
    switch (id) {
        case UBX_CFG_INF:
            return len != 0 && len % UBX_CFG_INF_BLOCK_LEN == 0;
        case UBX_CFG_CFG:
            return len == UBX_CFG_CFG_LEN || len == UBX_CFG_CFG_DEVICE_LEN;
        case UBX_CFG_RXM:
            return len == UBX_CFG_RXM_LEN;
        case UBX_CFG_NAVX5:
            return len == UBX_CFG_NAVX5_LEN;
        case UBX_CFG_ITFM:
            return len == UBX_CFG_ITFM_LEN;
        case UBX_CFG_PM2:
            return len == UBX_CFG_PM2_LEN;
        case UBX_CFG_PMS:
            return len == UBX_CFG_PMS_LEN;
        default:
            return false;
    }
}

// CFG-GNSS: msgVer 0, then one 8-byte block per system, gnssId first and
// bit 0 of the flags word enabling it. SBAS, QZSS and IMES change nothing
// here.
static bool cfg_gnss(struct neo_m8n_state *state, const uint8_t *p, uint16_t len) {
    static const struct {
        uint8_t id;
        uint8_t mask;
    } systems[] = {
        { UBX_GNSS_ID_GPS, UBX_GNSS_GPS },
        { UBX_GNSS_ID_GLONASS, UBX_GNSS_GLONASS },
        { UBX_GNSS_ID_BEIDOU, UBX_GNSS_BEIDOU },
        { UBX_GNSS_ID_GALILEO, UBX_GNSS_GALILEO },
    };
    uint8_t gnss = state->gnss;

    if (len < UBX_CFG_GNSS_HEADER_LEN || p[0] != 0 || len != UBX_CFG_GNSS_HEADER_LEN + UBX_CFG_GNSS_BLOCK_LEN * p[3]) {
        return false;
    }
    for (const uint8_t *b = &p[UBX_CFG_GNSS_HEADER_LEN]; b < p + len; b += UBX_CFG_GNSS_BLOCK_LEN) {
        for (size_t i = 0; i < sizeof(systems) / sizeof(systems[0]); i++) {
            if (b[0] != systems[i].id) {
                continue;
            }
            if ((get_u32(&b[4]) & UBX_CFG_GNSS_ENABLE) != 0) {
                gnss |= systems[i].mask;
            } else {
                gnss &= (uint8_t)~systems[i].mask;
            }
        }
    }
    state->gnss = gnss;
    return true;
}

static void enter_backup(struct neo_m8n_state *state, uint32_t duration_ms, bool rx_wake) {
    state->backup = true;
    state->backup_rx_wake = rx_wake;
    state->backup_until_us = duration_ms == 0 ? 0 : mclw_now_us() + (uint64_t)duration_ms * 1000u;
    nmea_drop_pending(state);
    mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_STATE, 1, 0, 0);
}

// Waking is a restart that keeps the configuration: the first fix comes one
// interval later.
static void leave_backup(struct neo_m8n_state *state, uint64_t now_us) {
    state->backup = false;
    state->parse = PARSE_SYNC_1;
    state->line_len = 0;
    state->next_fix_us = now_us + (uint64_t)state->rate_ms * 1000u;
    mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_STATE, 0, 0, 0);
}

// RXM-PMREQ, 8 bytes (duration, flags) or 16 (version 0, duration, flags,
// wakeupSources). Only a backup request does anything; the part sends no
// acknowledgement either way. Version 0 wakes on UART activity, as
// Meshtastic relies on when it sends 0xFF.
static void handle_pmreq(struct neo_m8n_state *state) {
    const uint8_t *p = state->payload;

    if (state->len == UBX_RXM_PMREQ_V0_LEN && (get_u32(&p[4]) & UBX_RXM_PMREQ_BACKUP) != 0) {
        enter_backup(state, get_u32(&p[0]), true);
    } else if (state->len == UBX_RXM_PMREQ_V1_LEN && p[0] == 0 && (get_u32(&p[8]) & UBX_RXM_PMREQ_BACKUP) != 0) {
        enter_backup(state, get_u32(&p[4]), (get_u32(&p[12]) & UBX_RXM_PMREQ_WAKE_UARTRX) != 0);
    }
}

// Every CFG message is acknowledged, a poll after its reply.
static void handle_cfg(struct neo_m8n_state *state) {
    const uint8_t *p = state->payload;

    switch (state->id) {
        case UBX_CFG_PRT:
            if (state->len == 0 || (state->len == 1 && p[0] == UBX_CFG_PRT_PORT_UART1)) {
                send_cfg_prt(state);
                send_ack(state, true, state->cls, state->id);
            } else if (state->len == UBX_CFG_PRT_LEN && p[0] == UBX_CFG_PRT_PORT_UART1) {
                uint32_t baud = get_u32(&p[8]);

                send_ack(state, true, state->cls, state->id);
                state->in_proto = get_u16(&p[12]);
                set_out_proto(state, get_u16(&p[14]));
                if (baud >= NEO_M8N_BAUD_MIN && baud <= NEO_M8N_BAUD_MAX) {
                    apply_baud(state, baud);
                }
            } else {
                send_ack(state, false, state->cls, state->id);
            }
            break;
        case UBX_CFG_MSG:
            if (state->len == UBX_CFG_MSG_POLL_LEN) {
                send_cfg_msg(state, p[0], p[1]);
                send_ack(state, true, state->cls, state->id);
            } else if (state->len == UBX_CFG_MSG_LEN || state->len == UBX_CFG_MSG_PORTS_LEN) {
                uint8_t *rate = msg_rate(state, p[0], p[1]);

                if (rate != NULL) {
                    *rate = state->len == UBX_CFG_MSG_LEN ? p[2] : p[2 + UBX_CFG_MSG_PORT_UART1];
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
                send_ack(state, true, state->cls, state->id);
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
        case UBX_CFG_NMEA:
            if ((state->len == UBX_CFG_NMEA_V0_SHORT_LEN || state->len == UBX_CFG_NMEA_V0_LEN ||
                    state->len == UBX_CFG_NMEA_V1_LEN) && nmea_version_valid(p[1])) {
                state->nmea_version = p[1];
                send_ack(state, true, state->cls, state->id);
            } else {
                send_ack(state, false, state->cls, state->id);
            }
            break;
        case UBX_CFG_GNSS:
            send_ack(state, cfg_gnss(state, p, state->len), state->cls, state->id);
            break;
        case UBX_CFG_NAV5:
            if (state->len == 0) {
                send_cfg_nav5(state);
                send_ack(state, true, state->cls, state->id);
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
            send_ack(state, cfg_accepted(state->id, state->len), state->cls, state->id);
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
        case UBX_CLASS_RXM:
            if (state->id == UBX_RXM_PMREQ) {
                handle_pmreq(state);
            } else {
                send_ack(state, false, state->cls, state->id);
            }
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

static void parse_ubx(struct neo_m8n_state *state, uint8_t byte) {
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

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static bool parse_rate(const char *s, uint8_t *out) {
    unsigned v = 0;

    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        v = v * 10u + (unsigned)(*s - '0');
        if (v > 255u) {
            return false;
        }
    }
    *out = (uint8_t)v;
    return true;
}

// $PUBX,40,msgId,rddc,rus1,rus2,rusb,rspi,reserved: the rate of a standard
// NMEA message on each port. UART1's applies.
static void nmea_pubx_rate(struct neo_m8n_state *state, char *const fields[], size_t count) {
    static const char ids[NMEA_STANDARD_COUNT][4] = { "GGA", "GLL", "GSA", "GSV", "RMC", "VTG" };
    uint8_t rate;

    if (count < 5 || !parse_rate(fields[4], &rate)) {
        return;
    }
    for (unsigned i = 0; i < NMEA_STANDARD_COUNT; i++) {
        if (strcmp(fields[2], ids[i]) == 0) {
            state->nmea_rate[i] = rate;
            mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_COMMAND,
                ((uint32_t)UBX_CLASS_NMEA << 8) | i, rate, 0);
        }
    }
}

// A complete input line, '$' through the checksum. Anything with a bad or
// missing checksum, and anything other than PUBX,40, is ignored.
static void nmea_input(struct neo_m8n_state *state) {
    char *line = state->line;
    size_t len = state->line_len;
    char *fields[9];
    size_t count = 0;
    uint8_t x = 0;

    if (len < 4 || line[len - 3] != '*' || hex_digit(line[len - 2]) < 0 || hex_digit(line[len - 1]) < 0) {
        return;
    }
    for (size_t i = 1; i < len - 3; i++) {
        x ^= (uint8_t)line[i];
    }
    if (x != (uint8_t)((hex_digit(line[len - 2]) << 4) | hex_digit(line[len - 1]))) {
        return;
    }
    line[len - 3] = '\0';
    fields[count++] = &line[1];
    for (size_t i = 1; i < len - 3 && count < sizeof(fields) / sizeof(fields[0]); i++) {
        if (line[i] == ',') {
            line[i] = '\0';
            fields[count++] = &line[i + 1];
        }
    }
    if (count >= 2 && strcmp(fields[0], "PUBX") == 0 && strcmp(fields[1], "40") == 0) {
        nmea_pubx_rate(state, fields, count);
    }
}

static void nmea_byte(struct neo_m8n_state *state, uint8_t byte) {
    if (byte == '\r' || byte == '\n') {
        if (!state->line_overflow) {
            nmea_input(state);
        }
        state->line_len = 0;
    } else if (byte == '$') {
        state->line[0] = '$';
        state->line_len = 1;
        state->line_overflow = false;
    } else if (state->line_len < NMEA_LINE_MAX) {
        state->line[state->line_len++] = (char)byte;
    } else {
        state->line_overflow = true;
    }
}

// One input stream carries both protocols: a byte inside a UBX frame
// belongs to it, a '$' outside one starts an NMEA line, and a UBX sync
// byte abandons a line.
static void rx_byte(struct neo_m8n_state *state, uint8_t byte) {
    if (state->parse != PARSE_SYNC_1) {
        parse_ubx(state, byte);
        return;
    }
    if (state->line_len != 0) {
        if (byte != UBX_SYNC_1) {
            nmea_byte(state, byte);
            return;
        }
        state->line_len = 0;
    }
    if (byte == '$' && (state->in_proto & UBX_PROTO_NMEA) != 0) {
        nmea_byte(state, byte);
    } else if ((state->in_proto & UBX_PROTO_UBX) != 0) {
        parse_ubx(state, byte);
    }
}

static void on_rx(struct mclw_uart *uart, const uint8_t *bytes, size_t len, void *ctx) {
    struct neo_m8n_state *state = ctx;

    (void)uart;
    if (state->backup) {
        // The bytes that wake the part are lost.
        if (state->backup_rx_wake && len != 0) {
            leave_backup(state, mclw_now_us());
        }
        return;
    }
    for (size_t i = 0; i < len; i++) {
        rx_byte(state, bytes[i]);
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
    state->hdop = NEO_M8N_DEFAULT_HDOP;
    state->vdop = NEO_M8N_DEFAULT_VDOP;
    state->utc_set_ms = ms_from_civil((const int64_t[6]){ 2015, 1, 1, 0, 0, 0 });
    state->utc_pending = true;
    state->rate_ms = NEO_M8N_DEFAULT_RATE_MS;
    state->nav_rate = NEO_M8N_DEFAULT_NAV_RATE;
    state->time_ref = NEO_M8N_DEFAULT_TIME_REF;
    state->in_proto = NEO_M8N_DEFAULT_IN_PROTO;
    state->out_proto = NEO_M8N_DEFAULT_OUT_PROTO;
    state->nav_pvt_rate = 0;
    memset(state->nmea_rate, 1, sizeof(state->nmea_rate));
    state->nmea_version = NEO_M8N_DEFAULT_NMEA_VERSION;
    state->gnss = NEO_M8N_DEFAULT_GNSS;
    state->dyn_model = NEO_M8N_DEFAULT_DYN_MODEL;
    state->running = true;
    state->backup = false;
    state->resets = 0;
    state->fixes = 0;
    state->sentences = 0;
    state->dropped = 0;
    state->epochs = 0;
    state->next_fix_us = 0;
    state->nmea_due = 0;
    state->nmea_slot = SLOT_END;
    state->parse = PARSE_SYNC_1;
    state->line_len = 0;
}

static int neo_m8n_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct neo_m8n_state *state = inst->state;
    struct mclw_uart_stats stats;

    state->uart = bindings[BINDING_UART].uart;
    mclw_hal_uart_stats(state->uart, &stats);
    state->tx_base = stats.bytes_tx;
    state->tx_queued = 0;
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
        case PROP_TIME: {
            int32_t utc[7];

            civil_from_ms(state->utc_pending ? state->utc_set_ms
                : state->utc_anchor_ms + (int64_t)((mclw_now_us() - state->utc_anchor_us) / 1000u), utc);
            for (size_t i = 0; i < 6; i++) {
                out->v[i] = utc[i];
            }
            return 0;
        }
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
        case PROP_HDOP:
            out->v[0] = state->hdop;
            return 0;
        case PROP_VDOP:
            out->v[0] = state->vdop;
            return 0;
        case PROP_PROTOCOLS:
            out->v[0] = state->in_proto;
            out->v[1] = state->out_proto;
            return 0;
        case PROP_NMEA_RATES:
            for (size_t i = 0; i < NMEA_STANDARD_COUNT; i++) {
                out->v[i] = state->nmea_rate[i];
            }
            return 0;
        case PROP_NMEA_VERSION:
            out->v[0] = state->nmea_version;
            return 0;
        case PROP_SENTENCES:
            out->v[0] = state->sentences;
            return 0;
        case PROP_DROPPED:
            out->v[0] = state->dropped;
            return 0;
        case PROP_GNSS:
            out->v[0] = state->gnss;
            return 0;
        case PROP_BACKUP:
            out->v[0] = state->backup ? 1 : 0;
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
        case PROP_TIME: {
            int64_t fields[6];

            for (size_t i = 0; i < 6; i++) {
                fields[i] = in->v[i];
            }
            state->utc_set_ms = ms_from_civil(fields);
            state->utc_pending = true;
            return 0;
        }
        case PROP_BAUD:
            apply_baud(state, (uint32_t)in->v[0]);
            return state->baud == (uint32_t)in->v[0] ? 0 : -EIO;
        case PROP_HDOP:
            state->hdop = (uint16_t)in->v[0];
            return 0;
        case PROP_VDOP:
            state->vdop = (uint16_t)in->v[0];
            return 0;
        default:
            return -EINVAL;
    }
}

// One epoch per fix interval, on a grid from the first so the UTC it
// carries stays whole; a jump of several intervals still produces one
// epoch, and the grid restarts from it. Between epochs, the epoch's NMEA
// leaves as the port drains.
static void neo_m8n_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct neo_m8n_state *state = inst->state;
    uint64_t rate_us = (uint64_t)state->rate_ms * 1000u;
    uint64_t epoch_us = now_us;

    if (state->backup) {
        if (state->backup_until_us != 0 && now_us >= state->backup_until_us) {
            leave_backup(state, now_us);
        }
        return;
    }
    if (now_us >= state->next_fix_us) {
        if (state->next_fix_us != 0 && now_us - state->next_fix_us < rate_us) {
            epoch_us = state->next_fix_us;
        }
        state->next_fix_us = epoch_us + rate_us;
        if (state->running) {
            state->epoch_utc_ms = utc_at_epoch(state, epoch_us);
            if (state->nav_pvt_rate != 0 && state->epochs % state->nav_pvt_rate == 0) {
                send_nav_pvt(state);
            }
            if ((state->out_proto & UBX_PROTO_NMEA) != 0) {
                nmea_epoch(state);
            }
            state->epochs++;
        }
    }
    nmea_pump(state);
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
