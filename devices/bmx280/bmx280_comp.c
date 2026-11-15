// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdint.h>

#include "bmx280_comp.h"

// The trimming words of the part the model is. T1..P9 are the example set
// Bosch published with earlier revisions of BST-BMP280-DS001 (section 3.12),
// H1..H6 typical of production BME280 parts; any realistic set works,
// since the raw words are found by inverting the formulas with them.
const struct bmx280_calib mclw_bmx280_calib = {
    .t1 = 27504,
    .t2 = 26435,
    .t3 = -1000,
    .p1 = 36477,
    .p2 = -10685,
    .p3 = 3024,
    .p4 = 2855,
    .p5 = 140,
    .p6 = -7,
    .p7 = 15500,
    .p8 = -14600,
    .p9 = 6000,
    .h1 = 75,
    .h2 = 362,
    .h3 = 0,
    .h4 = 313,
    .h5 = 50,
    .h6 = 30,
};

// The datasheets' code shifts signed values left (var1 * dig_P5 << 1 with
// var1 below zero under 25 degC); here those are multiplications, which give
// the same two's-complement result without the undefined behaviour.

int32_t bmx280_t_fine(const struct bmx280_calib *c, int32_t adc_t) {
    int32_t var1 = (((adc_t >> 3) - ((int32_t)c->t1 * 2)) * (int32_t)c->t2) >> 11;
    int32_t d = (adc_t >> 4) - (int32_t)c->t1;
    int32_t var2 = (((d * d) >> 12) * (int32_t)c->t3) >> 14;

    return var1 + var2;
}

int32_t bmx280_temperature(int32_t t_fine) {
    return (t_fine * 5 + 128) >> 8;
}

uint32_t bmx280_pressure_int32(const struct bmx280_calib *c, int32_t t_fine, int32_t adc_p) {
    int32_t var1 = (t_fine >> 1) - 64000;
    int32_t var2 = (((var1 >> 2) * (var1 >> 2)) >> 11) * (int32_t)c->p6;
    uint32_t p;

    var2 = var2 + var1 * (int32_t)c->p5 * 2;
    var2 = (var2 >> 2) + (int32_t)c->p4 * 65536;
    var1 = ((((int32_t)c->p3 * (((var1 >> 2) * (var1 >> 2)) >> 13)) >> 3)
        + (((int32_t)c->p2 * var1) >> 1)) >> 18;
    var1 = ((32768 + var1) * (int32_t)c->p1) >> 15;
    if (var1 == 0) {
        return 0;
    }
    p = ((uint32_t)(1048576 - adc_p) - (uint32_t)(var2 >> 12)) * 3125u;
    if (p < 0x80000000u) {
        p = (p << 1) / (uint32_t)var1;
    } else {
        p = (p / (uint32_t)var1) * 2u;
    }
    var1 = ((int32_t)c->p9 * (int32_t)(((p >> 3) * (p >> 3)) >> 13)) >> 12;
    var2 = ((int32_t)(p >> 2) * (int32_t)c->p8) >> 13;
    return (uint32_t)((int32_t)p + ((var1 + var2 + c->p7) >> 4));
}

uint32_t bmx280_pressure_int64(const struct bmx280_calib *c, int32_t t_fine, int32_t adc_p) {
    int64_t var1 = (int64_t)t_fine - 128000;
    int64_t var2 = var1 * var1 * (int64_t)c->p6;
    int64_t p;

    var2 = var2 + var1 * (int64_t)c->p5 * 131072;
    var2 = var2 + (int64_t)c->p4 * 34359738368;
    var1 = ((var1 * var1 * (int64_t)c->p3) >> 8) + var1 * (int64_t)c->p2 * 4096;
    var1 = ((((int64_t)1) << 47) + var1) * (int64_t)c->p1 >> 33;
    if (var1 == 0) {
        return 0;
    }
    p = 1048576 - adc_p;
    p = ((p * 2147483648) - var2) * 3125 / var1;
    var1 = ((int64_t)c->p9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = ((int64_t)c->p8 * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (int64_t)c->p7 * 16;
    return (uint32_t)p;
}

uint32_t bmx280_humidity(const struct bmx280_calib *c, int32_t t_fine, int32_t adc_h) {
    int32_t v = t_fine - 76800;

    v = (((adc_h * 16384) - ((int32_t)c->h4 * 1048576) - ((int32_t)c->h5 * v) + 16384) >> 15)
        * (((((((v * (int32_t)c->h6) >> 10) * (((v * (int32_t)c->h3) >> 11) + 32768)) >> 10)
            + 2097152) * (int32_t)c->h2 + 8192) >> 14);
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)c->h1) >> 4);
    v = v < 0 ? 0 : v;
    v = v > 419430400 ? 419430400 : v;
    return (uint32_t)(v >> 12);
}

static uint32_t magnitude(int64_t value) {
    return (uint32_t)(value < 0 ? -value : value);
}

// The compensated values rise (temperature, humidity) or fall (pressure)
// with the raw word, but the formulas floor at several places, so they are
// only nearly monotonic: a bisection lands next to the answer, and a scan
// of a few words either side finds the best.
#define SCAN_WORDS 48

static uint32_t clamp_low(uint32_t at) {
    return at > SCAN_WORDS ? at - SCAN_WORDS : 0u;
}

static uint32_t clamp_high(uint32_t at, uint32_t max) {
    return at + SCAN_WORDS < max ? at + SCAN_WORDS : max;
}

uint32_t bmx280_raw_temperature(const struct bmx280_calib *c, int32_t centi_c, int32_t *t_fine) {
    uint32_t lo = 0;
    uint32_t hi = BMX280_ADC_20_MAX;
    uint32_t best;
    uint32_t best_error = UINT32_MAX;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (bmx280_temperature(bmx280_t_fine(c, (int32_t)mid)) < centi_c) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    best = lo;
    for (uint32_t adc = clamp_low(lo); adc <= clamp_high(lo, BMX280_ADC_20_MAX); adc++) {
        uint32_t error = magnitude((int64_t)bmx280_temperature(bmx280_t_fine(c, (int32_t)adc)) - centi_c);
        if (error < best_error) {
            best_error = error;
            best = adc;
        }
    }
    *t_fine = bmx280_t_fine(c, (int32_t)best);
    return best;
}

uint32_t bmx280_raw_pressure(const struct bmx280_calib *c, int32_t t_fine, int32_t pa) {
    uint32_t lo = 0;
    uint32_t hi = BMX280_ADC_20_MAX;
    uint32_t best;
    uint32_t best_error = UINT32_MAX;
    uint32_t best_fine = UINT32_MAX;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if ((int64_t)bmx280_pressure_int32(c, t_fine, (int32_t)mid) > pa) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    best = lo;
    for (uint32_t adc = clamp_low(lo); adc <= clamp_high(lo, BMX280_ADC_20_MAX); adc++) {
        uint32_t error = magnitude((int64_t)bmx280_pressure_int32(c, t_fine, (int32_t)adc) - pa);
        uint32_t fine = magnitude((int64_t)bmx280_pressure_int64(c, t_fine, (int32_t)adc) - (int64_t)pa * 256);
        if (error < best_error || (error == best_error && fine < best_fine)) {
            best_error = error;
            best_fine = fine;
            best = adc;
        }
    }
    return best;
}

uint32_t bmx280_raw_humidity(const struct bmx280_calib *c, int32_t t_fine, uint32_t q10) {
    uint32_t lo = 0;
    uint32_t hi = BMX280_ADC_16_MAX;
    uint32_t best;
    uint32_t best_error = UINT32_MAX;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (bmx280_humidity(c, t_fine, (int32_t)mid) < q10) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    best = lo;
    for (uint32_t adc = clamp_low(lo); adc <= clamp_high(lo, BMX280_ADC_16_MAX); adc++) {
        uint32_t error = magnitude((int64_t)bmx280_humidity(c, t_fine, (int32_t)adc) - (int64_t)q10);
        if (error < best_error) {
            best_error = error;
            best = adc;
        }
    }
    return best;
}

int32_t bmx280_centi_c(int32_t milli_c) {
    return milli_c >= 0 ? (milli_c + 5) / 10 : -((5 - milli_c) / 10);
}

uint32_t bmx280_humidity_q10(int32_t milli_rh) {
    return (uint32_t)(((int64_t)milli_rh * 1024 + 500) / 1000);
}
