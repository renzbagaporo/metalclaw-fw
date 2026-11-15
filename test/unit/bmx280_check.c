// SPDX-License-Identifier: Apache-2.0 OR MIT
// Checks devices/bmx280/bmx280_comp.c on the host against a driver's side
// of the bus: the compensation code as the datasheets print it, shifts and
// all (BST-BMP280-DS001 3.11.3 and 8.2, BST-BME280-DS002 4.2.3), the code
// pico-examples' bmp280_i2c and bme280_spi carry.
//
//   bmx280_check                 sweeps the property ranges; prints "ok"
//   bmx280_check bmp280 PA MC    prints what bmp280_i2c prints for one reading
//   bmx280_check bme280 PA MC MRH  prints what bme280_spi prints

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bmx280_comp.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("failed: %s (line %d)\n", #cond, __LINE__); \
            exit(1); \
        } \
    } while (0)

static const struct bmx280_calib *cal = &mclw_bmx280_calib;
static int32_t t_fine;

static int32_t compensate_t(int32_t adc_T) {
    int32_t var1, var2, T;
    var1 = ((((adc_T >> 3) - ((int32_t)cal->t1 << 1))) * ((int32_t)cal->t2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)cal->t1)) * ((adc_T >> 4) - ((int32_t)cal->t1))) >> 12)
        * ((int32_t)cal->t3)) >> 14;
    t_fine = var1 + var2;
    T = (t_fine * 5 + 128) >> 8;
    return T;
}

static uint32_t compensate_p_int32(int32_t adc_P) {
    int32_t var1, var2;
    uint32_t p;
    var1 = (((int32_t)t_fine) >> 1) - (int32_t)64000;
    var2 = (((var1 >> 2) * (var1 >> 2)) >> 11) * ((int32_t)cal->p6);
    var2 = var2 + ((var1 * ((int32_t)cal->p5)) << 1);
    var2 = (var2 >> 2) + (((int32_t)cal->p4) << 16);
    var1 = (((cal->p3 * (((var1 >> 2) * (var1 >> 2)) >> 13)) >> 3) + ((((int32_t)cal->p2) * var1) >> 1)) >> 18;
    var1 = ((((32768 + var1)) * ((int32_t)cal->p1)) >> 15);
    if (var1 == 0) {
        return 0;
    }
    p = (((uint32_t)(((int32_t)1048576) - adc_P) - (uint32_t)(var2 >> 12))) * 3125;
    if (p < 0x80000000) {
        p = (p << 1) / ((uint32_t)var1);
    } else {
        p = (p / (uint32_t)var1) * 2;
    }
    var1 = (((int32_t)cal->p9) * ((int32_t)(((p >> 3) * (p >> 3)) >> 13))) >> 12;
    var2 = (((int32_t)(p >> 2)) * ((int32_t)cal->p8)) >> 13;
    p = (uint32_t)((int32_t)p + ((var1 + var2 + cal->p7) >> 4));
    return p;
}

static uint32_t compensate_p_int64(int32_t adc_P) {
    int64_t var1, var2, p;
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)cal->p6;
    var2 = var2 + ((var1 * (int64_t)cal->p5) << 17);
    var2 = var2 + (((int64_t)cal->p4) << 35);
    var1 = ((var1 * var1 * (int64_t)cal->p3) >> 8) + ((var1 * (int64_t)cal->p2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)cal->p1) >> 33;
    if (var1 == 0) {
        return 0;
    }
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)cal->p9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)cal->p8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)cal->p7) << 4);
    return (uint32_t)p;
}

static uint32_t compensate_h(int32_t adc_H) {
    int32_t v_x1_u32r;
    v_x1_u32r = (t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)cal->h4) << 20) - (((int32_t)cal->h5) * v_x1_u32r))
        + ((int32_t)16384)) >> 15) * (((((((v_x1_u32r * ((int32_t)cal->h6)) >> 10)
        * (((v_x1_u32r * ((int32_t)cal->h3)) >> 11) + ((int32_t)32768))) >> 10) + ((int32_t)2097152))
        * ((int32_t)cal->h2) + 8192) >> 14));
    v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) * ((int32_t)cal->h1)) >> 4));
    v_x1_u32r = (v_x1_u32r < 0 ? 0 : v_x1_u32r);
    v_x1_u32r = (v_x1_u32r > 419430400 ? 419430400 : v_x1_u32r);
    return (uint32_t)(v_x1_u32r >> 12);
}

static void raws(int32_t pa, int32_t mc, int32_t mrh, int32_t *adc_t, int32_t *adc_p, int32_t *adc_h) {
    int32_t fine;
    *adc_t = (int32_t)bmx280_raw_temperature(cal, bmx280_centi_c(mc), &fine);
    *adc_p = (int32_t)bmx280_raw_pressure(cal, fine, pa);
    *adc_h = (int32_t)bmx280_raw_humidity(cal, fine, bmx280_humidity_q10(mrh));
}

static int sweep(void) {
    static const int32_t temperatures[] = { -40000, -12345, 0, 4, 25000, 23450, 61875, 85000 };
    int64_t worst_p64 = 0;
    int64_t worst_h = 0;
    int32_t misses = 0;
    int32_t pressures = 0;

    for (int32_t mc = -40000; mc <= 85000; mc += 37) {
        int32_t fine;
        uint32_t adc = bmx280_raw_temperature(cal, bmx280_centi_c(mc), &fine);
        CHECK(adc <= BMX280_ADC_20_MAX);
        CHECK(compensate_t((int32_t)adc) == bmx280_centi_c(mc));
        CHECK(t_fine == fine);
    }
    for (size_t i = 0; i < sizeof(temperatures) / sizeof(temperatures[0]); i++) {
        for (int32_t pa = 30000; pa <= 110000; pa += 7) {
            int32_t adc_t, adc_p, adc_h;
            int64_t off;
            raws(pa, temperatures[i], 0, &adc_t, &adc_p, &adc_h);
            CHECK(adc_p >= 0 && (uint32_t)adc_p <= BMX280_ADC_20_MAX);
            compensate_t(adc_t);
            CHECK(bmx280_pressure_int32(cal, t_fine, adc_p) == compensate_p_int32(adc_p));
            off = (int64_t)compensate_p_int32(adc_p) - pa;
            CHECK(off >= -1 && off <= 1);
            misses += off != 0;
            pressures++;
            CHECK(bmx280_pressure_int64(cal, t_fine, adc_p) == compensate_p_int64(adc_p));
            off = (int64_t)compensate_p_int64(adc_p) - (int64_t)pa * 256;
            off = off < 0 ? -off : off;
            worst_p64 = off > worst_p64 ? off : worst_p64;
        }
        for (int32_t mrh = 0; mrh <= 100000; mrh += 11) {
            int32_t adc_t, adc_p, adc_h;
            int64_t off;
            raws(101325, temperatures[i], mrh, &adc_t, &adc_p, &adc_h);
            CHECK(adc_h >= 0 && (uint32_t)adc_h <= BMX280_ADC_16_MAX);
            compensate_t(adc_t);
            CHECK(bmx280_humidity(cal, t_fine, adc_h) == compensate_h(adc_h));
            off = (int64_t)compensate_h(adc_h) - (int64_t)bmx280_humidity_q10(mrh);
            off = off < 0 ? -off : off;
            worst_h = off > worst_h ? off : worst_h;
        }
    }
    // The 32-bit formula's integer steps skip about one pascal in a hundred
    // over the range (measured: 875 of 80001 at 25 degC, 2899 at -40 degC);
    // those come out one off and every other one is exact. The 64-bit one
    // lands within the two formulas' own disagreement, up to 3.1 Pa below
    // 0 degC and under 1 Pa from 25 degC up (measured); humidity within half
    // the step between neighbouring raw words, which grows from 10/1024 %RH
    // at -40 degC to 14/1024 at 85 degC with these trimming words.
    CHECK(misses * 50 < pressures);
    CHECK(worst_p64 <= 4 * 256);
    CHECK(worst_h <= 7);
    printf("ok\n");
    return 0;
}

int main(int argc, char **argv) {
    int32_t adc_t, adc_p, adc_h;

    if (argc == 1) {
        return sweep();
    }
    if (argc == 4 && strcmp(argv[1], "bmp280") == 0) {
        raws(atoi(argv[2]), atoi(argv[3]), 0, &adc_t, &adc_p, &adc_h);
        // bmp280_i2c.c: bmp280_convert_temp, bmp280_convert_pressure, then
        // printf("Pressure = %.3f kPa\n", pressure / 1000.f) and
        // printf("Temp. = %.2f C\n", temperature / 100.f).
        int32_t temperature = compensate_t(adc_t);
        int32_t pressure = (int32_t)compensate_p_int32(adc_p);
        printf("Pressure = %.3f kPa\n", (double)((float)pressure / 1000.f));
        printf("Temp. = %.2f C\n", (double)((float)temperature / 100.f));
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "bme280") == 0) {
        raws(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), &adc_t, &adc_p, &adc_h);
        // bme280_spi.c: compensate_temp, compensate_pressure,
        // compensate_humidity, then its three printf lines.
        int32_t temperature = compensate_t(adc_t);
        int32_t pressure = (int32_t)compensate_p_int32(adc_p);
        int32_t humidity = (int32_t)compensate_h(adc_h);
        printf("Humidity = %.2f%%\n", humidity / 1024.0);
        printf("Pressure = %dPa\n", pressure);
        printf("Temp. = %.2fC\n", temperature / 100.0);
        return 0;
    }
    printf("usage: bmx280_check [bmp280 PA MC | bme280 PA MC MRH]\n");
    return 2;
}
