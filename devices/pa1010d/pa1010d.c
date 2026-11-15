// SPDX-License-Identifier: Apache-2.0 OR MIT
// CDTop PA1010D on I2C. Sources: the CD-PA1010D Data Sheet V.03 (sentence
// formats, talker IDs, defaults), GlobalTop's "GPS NMEA over I2C Software
// Guide" v1.0 (the I2C buffer: 2, 3, 3.1, 3.2, 4) and GlobalTop's "PMTK
// command packet" Rev.A11 (packet format, PMTK 001, 010, 011, 101..104, 220,
// 314, 414, 514). See README.md for what each covers.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "pa1010d.h"

// What one fix interval's sentences may queue: a default epoch with 32
// satellites is about 900 bytes.
#define OUT_MAX 1024
#define SENTENCE_MAX 100
#define LINE_MAX 96
#define INPUT_RING 256
#define GSA_SVS 12
#define GSV_SVS_PER_PAGE 4
#define GLONASS_SVID_BASE 65

#define LATITUDE_MAX 900000000
#define LONGITUDE_MAX 1800000000
#define HEADING_MAX 35999999

enum {
    PROP_LATITUDE,
    PROP_LONGITUDE,
    PROP_ALTITUDE,
    PROP_SPEED,
    PROP_HEADING,
    PROP_SATELLITES,
    PROP_FIX,
    PROP_PDOP,
    PROP_HDOP,
    PROP_VDOP,
    PROP_TIME,
    PROP_GNSS,
    PROP_RATE_MS,
    PROP_NMEA_RATES,
    PROP_COMMANDS,
    PROP_SENTENCES,
    PROP_DROPPED,
    PROP_PACKETS,
};

enum {
    BINDING_I2C,
};

enum {
    SYS_GPS,
    SYS_GLONASS,
    SYS_COUNT,
};

struct pa1010d_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
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
    int64_t utc_set_ms;
    int64_t utc_anchor_ms;
    uint64_t utc_anchor_us;
    bool utc_pending;
    int64_t epoch_utc_ms;
    uint8_t gnss;
    uint16_t rate_ms;
    uint8_t rates[PA1010D_SENTENCES];
    uint32_t epochs;
    uint64_t next_fix_us;
    uint32_t commands;
    uint32_t sentences;
    uint32_t dropped;
    uint32_t packets;
    // The module's NMEA output not yet read; the image holds its first
    // `packet_len` bytes. A read is taken once the bus has been quiet for a
    // poll: bus bytes since the packet was loaded, less those written.
    uint8_t out[OUT_MAX];
    size_t out_len;
    size_t packet_len;
    bool reload;
    uint32_t bus_bytes;
    uint32_t loaded_bytes;
    uint32_t loaded_inputs;
    // Written bytes, from the engine's interrupt.
    volatile uint32_t inputs;
    volatile uint16_t in_head;
    uint16_t in_tail;
    uint32_t in_overruns;
    uint8_t in[INPUT_RING];
    uint8_t line_len;
    bool line_overflow;
    char line[LINE_MAX + 1];
};

static const struct mclw_property_desc pa1010d_properties[] = {
    { .name = "latitude", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "1e-7 deg",
      .min = -LATITUDE_MAX, .max = LATITUDE_MAX },
    { .name = "longitude", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "1e-7 deg",
      .min = -LONGITUDE_MAX, .max = LONGITUDE_MAX },
    { .name = "altitude", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mm",
      .min = -1000000, .max = 18000000 },
    { .name = "speed", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mm/s",
      .min = 0, .max = 515000 },
    { .name = "heading", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "1e-5 deg",
      .min = 0, .max = HEADING_MAX },
    { .name = "satellites", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = PA1010D_SATELLITES_MAX },
    { .name = "fix", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = 3 },
    { .name = "pdop", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "0.01",
      .min = 0, .max = 9999 },
    { .name = "hdop", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "0.01",
      .min = 0, .max = 9999 },
    { .name = "vdop", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "0.01",
      .min = 0, .max = 9999 },
    { .name = "time", .type = MCLW_TYPE_I32, .count = 6, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = 9999 },
    { .name = "gnss", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 1, .max = PA1010D_GNSS_GPS | PA1010D_GNSS_GLONASS },
    { .name = "rate_ms", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = "ms",
      .min = PA1010D_RATE_MS_MIN, .max = PA1010D_RATE_MS_MAX },
    { .name = "nmea_rates", .type = MCLW_TYPE_U32, .count = PA1010D_SENTENCES, .access = MCLW_ACCESS_RO,
      .unit = NULL, .min = 0, .max = 5 },
    { .name = "commands", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "sentences", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "dropped", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "packets", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

// Software guide 2: a 7-bit target at 0x10 by default, up to 400 kHz.
static const struct mclw_binding_desc pa1010d_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

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

static int64_t utc_at_epoch(struct pa1010d_state *s, uint64_t t_us) {
    if (s->utc_pending) {
        s->utc_anchor_ms = s->utc_set_ms;
        s->utc_anchor_us = t_us;
        s->utc_pending = false;
    }
    return s->utc_anchor_ms + (int64_t)((t_us - s->utc_anchor_us) / 1000u);
}

struct sentence {
    char s[SENTENCE_MAX];
    size_t len;
    bool overflow;
};

static void out_char(struct sentence *o, char c) {
    if (o->len < sizeof(o->s)) {
        o->s[o->len++] = c;
    } else {
        o->overflow = true;
    }
}

static void out_str(struct sentence *o, const char *str) {
    while (*str != '\0') {
        out_char(o, *str++);
    }
}

static void out_uint(struct sentence *o, uint64_t v, unsigned width) {
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

// v in units of 10^-decimals, never negative here.
static void out_fixed(struct sentence *o, uint64_t v, unsigned decimals) {
    uint64_t scale = 1;

    for (unsigned i = 0; i < decimals; i++) {
        scale *= 10u;
    }
    out_uint(o, v / scale, 1);
    out_char(o, '.');
    out_uint(o, v % scale, decimals);
}

static void out_head(struct sentence *o, const char *talker, const char *type) {
    o->len = 0;
    o->overflow = false;
    out_char(o, '$');
    out_str(o, talker);
    out_str(o, type);
}

// PMTK packet format (Rev.A11, "Packet Format"): '*', the XOR of everything
// between '$' and '*' as two hex digits, CR LF. NMEA sentences close alike.
static size_t out_finish(struct sentence *o) {
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

static bool has_position(const struct pa1010d_state *s) {
    return s->fix >= 2;
}

// Datasheet Table-2: GP or GL while one system is searched, GN for GGA, RMC
// and VTG while both are; GSA and GSV always carry their own system's.
static const char *main_talker(const struct pa1010d_state *s) {
    if (s->gnss == PA1010D_GNSS_GPS) {
        return "GP";
    }
    return s->gnss == PA1010D_GNSS_GLONASS ? "GL" : "GN";
}

static bool sys_present(const struct pa1010d_state *s, unsigned sys) {
    return (s->gnss & (sys == SYS_GPS ? PA1010D_GNSS_GPS : PA1010D_GNSS_GLONASS)) != 0;
}

static unsigned sys_satellites(const struct pa1010d_state *s, unsigned sys) {
    if (!sys_present(s, sys)) {
        return 0;
    }
    if (s->gnss == (PA1010D_GNSS_GPS | PA1010D_GNSS_GLONASS)) {
        return sys == SYS_GPS ? (s->satellites + 1u) / 2u : s->satellites / 2u;
    }
    return s->satellites;
}

static unsigned svid(unsigned sys, unsigned k) {
    return sys == SYS_GPS ? 1u + k : GLONASS_SVID_BASE + k;
}

static void out_time(struct sentence *o, const int32_t utc[7]) {
    out_uint(o, (uint64_t)utc[3], 2);
    out_uint(o, (uint64_t)utc[4], 2);
    out_uint(o, (uint64_t)utc[5], 2);
    out_char(o, '.');
    out_uint(o, (uint64_t)utc[6], 3);
}

// ddmm.mmmm,N or dddmm.mmmm,E (Table-3, Table-10): minutes to 1e-4.
static void out_coordinate(struct sentence *o, int32_t v, unsigned degree_digits, char positive, char negative) {
    uint32_t mag = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    uint32_t degrees = mag / 10000000u;
    uint32_t minutes_e4 = ((mag % 10000000u) * 60u + 500u) / 1000u;

    if (minutes_e4 >= 600000u) {
        degrees++;
        minutes_e4 -= 600000u;
    }
    out_uint(o, degrees, degree_digits);
    out_uint(o, minutes_e4 / 10000u, 2);
    out_char(o, '.');
    out_uint(o, minutes_e4 % 10000u, 4);
    out_char(o, ',');
    out_char(o, v < 0 ? negative : positive);
}

static void out_position(struct sentence *o, const struct pa1010d_state *s) {
    out_coordinate(o, s->latitude, 2, 'N', 'S');
    out_char(o, ',');
    out_coordinate(o, s->longitude, 3, 'E', 'W');
}

static uint64_t speed_centi_knots(const struct pa1010d_state *s) {
    return ((uint64_t)s->speed * 360u + 926u) / 1852u;
}

static uint64_t speed_centi_kmh(const struct pa1010d_state *s) {
    return ((uint64_t)s->speed * 36u + 50u) / 100u;
}

static void out_course(struct sentence *o, const struct pa1010d_state *s) {
    uint32_t centi = ((uint32_t)s->heading + 500u) / 1000u;

    out_fixed(o, centi >= 36000u ? centi - 36000u : centi, 2);
}

static char mode_indicator(const struct pa1010d_state *s) {
    return has_position(s) ? 'A' : 'N';
}

// GGA, Table-3: hhmmss.sss, ddmm.mmmm, position fix indicator (Table-4),
// satellites used, HDOP, MSL altitude in metres to 0.1 and the geoidal
// separation, which the model keeps at 0.
static void build_gga(const struct pa1010d_state *s, const int32_t utc[7], struct sentence *o) {
    bool position = has_position(s);

    out_head(o, main_talker(s), "GGA,");
    out_time(o, utc);
    out_char(o, ',');
    if (position) {
        out_position(o, s);
    } else {
        out_str(o, ",,,");
    }
    out_char(o, ',');
    out_char(o, position ? '1' : '0');
    out_char(o, ',');
    out_uint(o, position ? s->satellites : 0, 1);
    out_char(o, ',');
    if (position) {
        int64_t alt = s->altitude;
        out_fixed(o, s->hdop, 2);
        out_char(o, ',');
        if (alt < 0) {
            out_char(o, '-');
            alt = -alt;
        }
        out_fixed(o, (uint64_t)(alt + 50) / 100u, 1);
        out_str(o, ",M,0.0,M,,");
    } else {
        out_str(o, ",,M,,M,,");
    }
}

// GSA, Table-5 to 7: mode A, 1 no fix / 2 2D / 3 3D, twelve SV slots, the
// three DOPs.
static void build_gsa(const struct pa1010d_state *s, unsigned sys, struct sentence *o) {
    bool position = has_position(s);
    unsigned used = position ? sys_satellites(s, sys) : 0;

    out_head(o, sys == SYS_GPS ? "GP" : "GL", "GSA,A,");
    out_char(o, position ? (char)('0' + s->fix) : '1');
    out_char(o, ',');
    for (unsigned k = 0; k < GSA_SVS; k++) {
        if (k < used) {
            out_uint(o, svid(sys, k), 2);
        }
        out_char(o, ',');
    }
    if (position) {
        out_fixed(o, s->pdop, 2);
        out_char(o, ',');
        out_fixed(o, s->hdop, 2);
        out_char(o, ',');
        out_fixed(o, s->vdop, 2);
    } else {
        out_str(o, ",,");
    }
}

static unsigned gsv_pages(const struct pa1010d_state *s, unsigned sys) {
    unsigned n = sys_satellites(s, sys);

    return n == 0 ? 1 : (n + GSV_SVS_PER_PAGE - 1) / GSV_SVS_PER_PAGE;
}

// GSV, Table-8 and 9: synthetic satellites, GPS PRNs from 1 and GLONASS
// from 65, each with its own elevation, azimuth and SNR.
static void build_gsv(const struct pa1010d_state *s, unsigned sys, unsigned page, struct sentence *o) {
    unsigned n = sys_satellites(s, sys);
    unsigned first = page * GSV_SVS_PER_PAGE;

    out_head(o, sys == SYS_GPS ? "GP" : "GL", "GSV,");
    out_uint(o, gsv_pages(s, sys), 1);
    out_char(o, ',');
    out_uint(o, page + 1u, 1);
    out_char(o, ',');
    out_uint(o, n, 2);
    for (unsigned k = first; k < n && k < first + GSV_SVS_PER_PAGE; k++) {
        out_char(o, ',');
        out_uint(o, svid(sys, k), 2);
        out_char(o, ',');
        out_uint(o, 10u + (k * 23u + sys * 11u) % 80u, 2);
        out_char(o, ',');
        out_uint(o, (k * 77u + sys * 41u) % 360u, 3);
        out_char(o, ',');
        out_uint(o, 25u + (k * 7u + sys * 3u) % 25u, 2);
    }
}

// RMC, Table-10: speed in knots and course to 0.01, ddmmyy, no magnetic
// variation (the second example's empty fields), mode A or N.
static void build_rmc(const struct pa1010d_state *s, const int32_t utc[7], struct sentence *o) {
    bool position = has_position(s);

    out_head(o, main_talker(s), "RMC,");
    out_time(o, utc);
    out_char(o, ',');
    out_char(o, position ? 'A' : 'V');
    out_char(o, ',');
    if (position) {
        out_position(o, s);
    } else {
        out_str(o, ",,,");
    }
    out_char(o, ',');
    out_fixed(o, position ? speed_centi_knots(s) : 0, 2);
    out_char(o, ',');
    if (position) {
        out_course(o, s);
    } else {
        out_str(o, "0.00");
    }
    out_char(o, ',');
    out_uint(o, (uint64_t)utc[2], 2);
    out_uint(o, (uint64_t)utc[1], 2);
    out_uint(o, (uint64_t)(utc[0] % 100), 2);
    out_str(o, ",,,");
    out_char(o, mode_indicator(s));
}

// VTG, its table: course true, magnetic empty, knots and km/h to 0.01.
static void build_vtg(const struct pa1010d_state *s, struct sentence *o) {
    bool position = has_position(s);

    out_head(o, main_talker(s), "VTG,");
    if (position) {
        out_course(o, s);
    } else {
        out_str(o, "0.00");
    }
    out_str(o, ",T,,M,");
    out_fixed(o, position ? speed_centi_knots(s) : 0, 2);
    out_str(o, ",N,");
    out_fixed(o, position ? speed_centi_kmh(s) : 0, 2);
    out_str(o, ",K,");
    out_char(o, mode_indicator(s));
}

// GLL, PMTK 314 slot 0 (off by default): position, time, status, mode.
static void build_gll(const struct pa1010d_state *s, const int32_t utc[7], struct sentence *o) {
    bool position = has_position(s);

    out_head(o, main_talker(s), "GLL,");
    if (position) {
        out_position(o, s);
    } else {
        out_str(o, ",,,");
    }
    out_char(o, ',');
    out_time(o, utc);
    out_char(o, ',');
    out_char(o, position ? 'A' : 'V');
    out_char(o, ',');
    out_char(o, mode_indicator(s));
}

static uint32_t count_lines(const uint8_t *bytes, size_t len) {
    uint32_t n = 0;

    for (size_t i = 0; i < len; i++) {
        n += bytes[i] == '\n' ? 1u : 0u;
    }
    return n;
}

static void queue(struct pa1010d_state *s, struct sentence *o, bool nmea) {
    size_t len = out_finish(o);

    if (len == 0 || s->out_len + len > sizeof(s->out)) {
        s->dropped++;
        return;
    }
    memcpy(&s->out[s->out_len], o->s, len);
    s->out_len += len;
    s->reload = true;
    if (nmea) {
        s->sentences++;
    }
}

// The image is the packet: the first queued bytes, at most 254, then <LF>
// to the end of the 256-byte image (software guide 3.1, 3.2).
static void load_packet(struct pa1010d_state *s, uint32_t bus_bytes) {
    size_t n = s->out_len < PA1010D_PACKET_NMEA_MAX ? s->out_len : PA1010D_PACKET_NMEA_MAX;
    uint32_t critical = mclw_hal_critical_enter();

    for (size_t i = 0; i < MCLW_REGMAP_BANK_SIZE; i++) {
        s->map.value[0][i] = i < n ? s->out[i] : PA1010D_GARBAGE;
    }
    s->packet_len = n;
    s->loaded_bytes = bus_bytes;
    s->loaded_inputs = s->inputs;
    mclw_hal_critical_exit(critical);
    s->reload = false;
}

static void consume(struct pa1010d_state *s, size_t n) {
    memmove(s->out, &s->out[n], s->out_len - n);
    s->out_len -= n;
    s->reload = true;
}

// A new fix replaces what the master had not read of the last one (software
// guide 3.2, packet type 3).
static void epoch(struct pa1010d_state *s) {
    int32_t utc[7];
    struct sentence o;

    s->dropped += count_lines(s->out, s->out_len);
    s->out_len = 0;
    s->reload = true;
    civil_from_ms(s->epoch_utc_ms, utc);
    for (unsigned msg = 0; msg < PA1010D_SENTENCES; msg++) {
        bool due = s->rates[msg] != 0 && s->epochs % s->rates[msg] == 0;

        if (!due) {
            continue;
        }
        switch (msg) {
            case PA1010D_GGA:
                build_gga(s, utc, &o);
                queue(s, &o, true);
                break;
            case PA1010D_GSA:
                for (unsigned sys = 0; sys < SYS_COUNT; sys++) {
                    if (sys_present(s, sys)) {
                        build_gsa(s, sys, &o);
                        queue(s, &o, true);
                    }
                }
                break;
            case PA1010D_GSV:
                for (unsigned sys = 0; sys < SYS_COUNT; sys++) {
                    for (unsigned page = 0; sys_present(s, sys) && page < gsv_pages(s, sys); page++) {
                        build_gsv(s, sys, page, &o);
                        queue(s, &o, true);
                    }
                }
                break;
            default:
                break;
        }
    }
    // The datasheet's order: GGA, GSA, GSV, then RMC and VTG; GLL last.
    if (s->rates[PA1010D_RMC] != 0 && s->epochs % s->rates[PA1010D_RMC] == 0) {
        build_rmc(s, utc, &o);
        queue(s, &o, true);
    }
    if (s->rates[PA1010D_VTG] != 0 && s->epochs % s->rates[PA1010D_VTG] == 0) {
        build_vtg(s, &o);
        queue(s, &o, true);
    }
    if (s->rates[PA1010D_GLL] != 0 && s->epochs % s->rates[PA1010D_GLL] == 0) {
        build_gll(s, utc, &o);
        queue(s, &o, true);
    }
}

static void reply(struct pa1010d_state *s, const char *body) {
    struct sentence o;

    out_head(&o, "", body);
    queue(s, &o, false);
}

static void ack(struct pa1010d_state *s, unsigned type, unsigned flag) {
    struct sentence o;

    out_head(&o, "PMTK001,", "");
    out_uint(&o, type, 3);
    out_char(&o, ',');
    out_uint(&o, flag, 1);
    queue(s, &o, false);
}

// PMTK 314's default, as $PMTK314,-1 restores it: the five sentences
// datasheet Table-1 lists, every fix; GLL off.
static void default_rates(struct pa1010d_state *s) {
    memset(s->rates, 1, sizeof(s->rates));
    s->rates[PA1010D_GLL] = 0;
}

static void factory(struct pa1010d_state *s) {
    default_rates(s);
    s->rate_ms = PA1010D_RATE_MS_DEFAULT;
    s->gnss = PA1010D_GNSS_GPS | PA1010D_GNSS_GLONASS;
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

// A decimal field, optionally signed; false for anything else.
static bool parse_int(const char *f, long *out) {
    long v = 0;
    bool negative = *f == '-';

    if (negative) {
        f++;
    }
    while (*f == ' ') {
        f++;
    }
    if (*f == '\0') {
        return false;
    }
    for (; *f != '\0'; f++) {
        if (*f < '0' || *f > '9' || v > 100000) {
            return false;
        }
        v = v * 10 + (*f - '0');
    }
    *out = negative ? -v : v;
    return true;
}

// $PMTK314: up to 19 fields, each 0 (off) to 5 (every fifth fix); fields
// the packet leaves out are off. -1 alone restores the default.
static unsigned pmtk314(struct pa1010d_state *s, char *const fields[], size_t count) {
    uint8_t rates[PA1010D_SENTENCES] = { 0 };
    long v;

    if (count == 1 && parse_int(fields[0], &v) && v == -1) {
        default_rates(s);
        return 3;
    }
    if (count == 0 || count > PA1010D_PMTK314_FIELDS) {
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (!parse_int(fields[i], &v) || v < 0 || v > 5) {
            return 2;
        }
        if (i < PA1010D_SENTENCES) {
            rates[i] = (uint8_t)v;
        }
    }
    memcpy(s->rates, rates, sizeof(rates));
    return 3;
}

static void pmtk414(struct pa1010d_state *s) {
    struct sentence o;

    out_head(&o, "PMTK514", "");
    for (unsigned i = 0; i < PA1010D_PMTK314_FIELDS; i++) {
        out_char(&o, ',');
        out_uint(&o, i < PA1010D_SENTENCES ? s->rates[i] : 0, 1);
    }
    queue(s, &o, false);
}

static void restart(struct pa1010d_state *s, bool full) {
    if (full) {
        factory(s);
    }
    s->epochs = 0;
    reply(s, "PMTK010,001");
    reply(s, "PMTK011,MTKGPS");
}

// One PMTK packet, '$' to the checksum digits. A bad checksum is ignored;
// a packet type the model does not know is acknowledged as unsupported.
static void pmtk(struct pa1010d_state *s, char *line, size_t len) {
    char *star = memchr(line, '*', len);
    char *fields[PA1010D_PMTK314_FIELDS + 2];
    size_t count = 0;
    uint8_t x = 0;
    unsigned type;
    long v;

    if (star == NULL || star + 3 != line + len || len < 9 || strncmp(line, "$PMTK", 5) != 0) {
        return;
    }
    for (char *p = line + 1; p < star; p++) {
        x ^= (uint8_t)*p;
    }
    if (hex_digit(star[1]) < 0 || hex_digit(star[2]) < 0 ||
        (uint8_t)(hex_digit(star[1]) * 16 + hex_digit(star[2])) != x) {
        return;
    }
    *star = '\0';
    if (line[5] < '0' || line[5] > '9' || line[6] < '0' || line[6] > '9' || line[7] < '0' || line[7] > '9' ||
        (line[8] != ',' && line[8] != '\0')) {
        return;
    }
    type = (unsigned)((line[5] - '0') * 100 + (line[6] - '0') * 10 + (line[7] - '0'));
    s->commands++;
    for (char *p = line + 8; *p == ',' && count < sizeof(fields) / sizeof(fields[0]); count++) {
        *p++ = '\0';
        fields[count] = p;
        while (*p != ',' && *p != '\0') {
            p++;
        }
    }
    mclw_trace(mclw_instance_from_state(s), MCLW_TRACE_COMMAND, type, (uint16_t)count, 0);
    switch (type) {
        case 0:
            ack(s, type, 3);
            break;
        case 101:
        case 102:
        case 103:
        case 104:
            restart(s, type == 104);
            break;
        case 220:
            if (count == 1 && parse_int(fields[0], &v) && v >= PA1010D_RATE_MS_MIN && v <= PA1010D_RATE_MS_MAX) {
                s->rate_ms = (uint16_t)v;
                ack(s, type, 3);
            } else {
                ack(s, type, 2);
            }
            break;
        case 314:
            ack(s, type, pmtk314(s, fields, count));
            break;
        case 353: {
            // Datasheet notes to RMC, GGA and VTG: $PMTK353,1,0,... searches
            // GPS alone, $PMTK353,0,1,... GLONASS alone. Galileo and BeiDou
            // need other firmware.
            long gps;
            long glonass;
            bool other = false;

            for (size_t i = 2; i < count; i++) {
                other = other || !parse_int(fields[i], &v) || v != 0;
            }
            if (count >= 2 && parse_int(fields[0], &gps) && parse_int(fields[1], &glonass) && !other &&
                (gps | glonass) != 0 && (gps & ~1L) == 0 && (glonass & ~1L) == 0) {
                s->gnss = (uint8_t)((gps ? PA1010D_GNSS_GPS : 0) | (glonass ? PA1010D_GNSS_GLONASS : 0));
                ack(s, type, 3);
            } else {
                ack(s, type, 2);
            }
            break;
        }
        case 414:
            pmtk414(s);
            break;
        default:
            ack(s, type, 1);
            break;
    }
}

static void input_byte(struct pa1010d_state *s, uint8_t c) {
    if (c == '$') {
        s->line_len = 0;
        s->line_overflow = false;
        s->line[s->line_len++] = '$';
        return;
    }
    if (s->line_len == 0) {
        return;
    }
    if (c == '\r' || c == '\n') {
        if (!s->line_overflow) {
            s->line[s->line_len] = '\0';
            pmtk(s, s->line, s->line_len);
        }
        s->line_len = 0;
        return;
    }
    if (s->line_len < LINE_MAX) {
        s->line[s->line_len++] = (char)c;
    } else {
        s->line_overflow = true;
    }
}

// Interrupt context: the engine hands over each byte the master writes.
static void MCLW_RAM_FUNC(pa1010d_on_input)(struct mclw_regmap *map, uint8_t value, void *ctx) {
    struct pa1010d_state *s = ctx;
    uint16_t head = s->in_head;
    uint16_t next = (uint16_t)((head + 1u) % INPUT_RING);

    (void)map;
    s->inputs++;
    if (next == s->in_tail) {
        s->in_overruns++;
        return;
    }
    s->in[head] = value;
    s->in_head = next;
}

static void drain_input(struct pa1010d_state *s) {
    for (;;) {
        uint32_t critical = mclw_hal_critical_enter();
        uint16_t head = s->in_head;
        mclw_hal_critical_exit(critical);
        if (s->in_tail == head) {
            return;
        }
        input_byte(s, s->in[s->in_tail]);
        s->in_tail = (uint16_t)((s->in_tail + 1u) % INPUT_RING);
    }
}

// Once the bus has been quiet for a poll, what was read of the packet since
// it was loaded is gone, and the next packet goes up; software guide 3
// gives the module 2 ms for that.
static void service_bus(struct pa1010d_state *s) {
    struct mclw_i2c_target_stats stats;
    uint32_t inputs;

    mclw_hal_i2c_target_stats(s->i2c, &stats);
    if (stats.bytes != s->bus_bytes) {
        s->bus_bytes = stats.bytes;
        return;
    }
    {
        uint32_t critical = mclw_hal_critical_enter();
        inputs = s->inputs;
        mclw_hal_critical_exit(critical);
    }
    uint32_t read = (stats.bytes - s->loaded_bytes) - (inputs - s->loaded_inputs);
    if (read != 0) {
        size_t taken = read < s->packet_len ? read : s->packet_len;

        s->packets++;
        consume(s, taken);
    }
    if (s->reload || read != 0) {
        load_packet(s, stats.bytes);
    }
}

static void power_on(struct pa1010d_state *s) {
    s->latitude = 0;
    s->longitude = 0;
    s->altitude = 0;
    s->speed = 0;
    s->heading = 0;
    s->satellites = 0;
    s->fix = 0;
    s->pdop = 250;
    s->hdop = 150;
    s->vdop = 200;
    s->utc_set_ms = ms_from_civil((const int64_t[6]){ 1980, 1, 6, 0, 0, 0 });
    s->utc_pending = true;
    factory(s);
    s->epochs = 0;
    s->next_fix_us = 0;
    s->commands = 0;
    s->sentences = 0;
    s->dropped = 0;
    s->packets = 0;
    s->out_len = 0;
    s->line_len = 0;
    s->reload = true;
}

static int pa1010d_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct pa1010d_state *s = inst->state;
    struct mclw_i2c_target_stats stats;

    s->i2c = bindings[BINDING_I2C].i2c;
    power_on(s);
    mclw_regmap_init(&s->map, 1, 0, PA1010D_GARBAGE);
    mclw_regmap_set_on_input(&s->map, pa1010d_on_input, s);
    s->in_head = 0;
    s->in_tail = 0;
    s->inputs = 0;
    mclw_hal_i2c_target_stats(s->i2c, &stats);
    s->bus_bytes = stats.bytes;
    load_packet(s, stats.bytes);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void pa1010d_deinit(struct mclw_instance *inst) {
    struct pa1010d_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

// Without backup power the module forgets every setting (Rev.A11, note to
// PMTK 010): a reset is a power cycle.
static int pa1010d_reset(struct mclw_instance *inst, bool factory_reset) {
    struct pa1010d_state *s = inst->state;
    struct mclw_i2c_target_stats stats;

    (void)factory_reset;
    power_on(s);
    mclw_hal_i2c_target_stats(s->i2c, &stats);
    s->bus_bytes = stats.bytes;
    load_packet(s, stats.bytes);
    return 0;
}

static int pa1010d_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct pa1010d_state *s = inst->state;

    switch (property) {
        case PROP_LATITUDE:
            out->v[0] = s->latitude;
            return 0;
        case PROP_LONGITUDE:
            out->v[0] = s->longitude;
            return 0;
        case PROP_ALTITUDE:
            out->v[0] = s->altitude;
            return 0;
        case PROP_SPEED:
            out->v[0] = s->speed;
            return 0;
        case PROP_HEADING:
            out->v[0] = s->heading;
            return 0;
        case PROP_SATELLITES:
            out->v[0] = s->satellites;
            return 0;
        case PROP_FIX:
            out->v[0] = s->fix;
            return 0;
        case PROP_PDOP:
            out->v[0] = s->pdop;
            return 0;
        case PROP_HDOP:
            out->v[0] = s->hdop;
            return 0;
        case PROP_VDOP:
            out->v[0] = s->vdop;
            return 0;
        case PROP_TIME: {
            int32_t utc[7];

            civil_from_ms(s->utc_pending ? s->utc_set_ms : s->epoch_utc_ms, utc);
            for (size_t i = 0; i < 6; i++) {
                out->v[i] = utc[i];
            }
            return 0;
        }
        case PROP_GNSS:
            out->v[0] = s->gnss;
            return 0;
        case PROP_RATE_MS:
            out->v[0] = s->rate_ms;
            return 0;
        case PROP_NMEA_RATES:
            for (size_t i = 0; i < PA1010D_SENTENCES; i++) {
                out->v[i] = s->rates[i];
            }
            return 0;
        case PROP_COMMANDS:
            out->v[0] = s->commands;
            return 0;
        case PROP_SENTENCES:
            out->v[0] = s->sentences;
            return 0;
        case PROP_DROPPED:
            out->v[0] = s->dropped;
            return 0;
        case PROP_PACKETS:
            out->v[0] = s->packets;
            return 0;
        default:
            return -EINVAL;
    }
}

static int pa1010d_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct pa1010d_state *s = inst->state;

    switch (property) {
        case PROP_LATITUDE:
            s->latitude = (int32_t)in->v[0];
            return 0;
        case PROP_LONGITUDE:
            s->longitude = (int32_t)in->v[0];
            return 0;
        case PROP_ALTITUDE:
            s->altitude = (int32_t)in->v[0];
            return 0;
        case PROP_SPEED:
            s->speed = (uint32_t)in->v[0];
            return 0;
        case PROP_HEADING:
            s->heading = (int32_t)in->v[0];
            return 0;
        case PROP_SATELLITES:
            s->satellites = (uint8_t)in->v[0];
            return 0;
        case PROP_FIX:
            s->fix = (uint8_t)in->v[0];
            return 0;
        case PROP_PDOP:
            s->pdop = (uint16_t)in->v[0];
            return 0;
        case PROP_HDOP:
            s->hdop = (uint16_t)in->v[0];
            return 0;
        case PROP_VDOP:
            s->vdop = (uint16_t)in->v[0];
            return 0;
        case PROP_TIME: {
            int64_t fields[6];

            for (size_t i = 0; i < 6; i++) {
                fields[i] = in->v[i];
            }
            if (fields[1] < 1 || fields[1] > 12 || fields[2] < 1 || fields[2] > 31 || fields[3] > 23 ||
                fields[4] > 59 || fields[5] > 59) {
                return -EINVAL;
            }
            s->utc_set_ms = ms_from_civil(fields);
            s->utc_pending = true;
            return 0;
        }
        case PROP_GNSS:
            s->gnss = (uint8_t)in->v[0];
            return 0;
        default:
            return -EINVAL;
    }
}

// One fix per interval on a grid from the first, as neo_m8n keeps it: a jump
// of several intervals makes one fix and restarts the grid.
static void pa1010d_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct pa1010d_state *s = inst->state;
    uint64_t rate_us = (uint64_t)s->rate_ms * 1000u;
    uint64_t epoch_us = now_us;

    if (now_us < s->next_fix_us) {
        return;
    }
    if (s->next_fix_us != 0 && now_us - s->next_fix_us < rate_us) {
        epoch_us = s->next_fix_us;
    }
    s->next_fix_us = epoch_us + rate_us;
    s->epoch_utc_ms = utc_at_epoch(s, epoch_us);
    epoch(s);
    s->epochs++;
}

static void pa1010d_poll(struct mclw_instance *inst) {
    struct pa1010d_state *s = inst->state;

    drain_input(s);
    service_bus(s);
}

static const struct mclw_model_ops pa1010d_ops = {
    .init = pa1010d_init,
    .deinit = pa1010d_deinit,
    .reset = pa1010d_reset,
    .get = pa1010d_get,
    .set = pa1010d_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = pa1010d_time_advanced,
    .poll = pa1010d_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_pa1010d = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "pa1010d",
    .family = "pa1010d",
    .config = NULL,
    .properties = pa1010d_properties,
    .property_count = sizeof(pa1010d_properties) / sizeof(pa1010d_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = pa1010d_bindings,
    .binding_count = sizeof(pa1010d_bindings) / sizeof(pa1010d_bindings[0]),
    .state_size = sizeof(struct pa1010d_state),
    .ops = &pa1010d_ops,
};
