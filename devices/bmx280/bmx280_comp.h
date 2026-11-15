// SPDX-License-Identifier: Apache-2.0 OR MIT
// The BMP280 and BME280 compensation formulas, as the datasheets give them,
// and their inverses: the raw ADC word that makes a driver's compensation
// land on a chosen value. Plain C with no platform headers, so the same
// source is checked on the host (test/unit/test_part_models.py).

#ifndef MCLW_DEVICE_BMX280_COMP_H
#define MCLW_DEVICE_BMX280_COMP_H

#include <stdint.h>

// The trimming words (BST-BMP280-DS001 table 17, BST-BME280-DS002 table 16).
struct bmx280_calib {
    uint16_t t1;
    int16_t t2;
    int16_t t3;
    uint16_t p1;
    int16_t p2;
    int16_t p3;
    int16_t p4;
    int16_t p5;
    int16_t p6;
    int16_t p7;
    int16_t p8;
    int16_t p9;
    uint8_t h1;
    int16_t h2;
    uint8_t h3;
    int16_t h4;
    int16_t h5;
    int8_t h6;
};

extern const struct bmx280_calib mclw_bmx280_calib;

#define BMX280_ADC_20_MAX 0xfffffu
#define BMX280_ADC_16_MAX 0xffffu

// BST-BMP280-DS001 3.11.3 / BST-BME280-DS002 4.2.3, bit for bit.
int32_t bmx280_t_fine(const struct bmx280_calib *c, int32_t adc_t);
// 0.01 degC.
int32_t bmx280_temperature(int32_t t_fine);
// Pa, the 32-bit fixed-point formula (DS001 8.2, DS002 8.2).
uint32_t bmx280_pressure_int32(const struct bmx280_calib *c, int32_t t_fine, int32_t adc_p);
// Pa in Q24.8, the 64-bit formula (DS001 3.11.3, DS002 4.2.3).
uint32_t bmx280_pressure_int64(const struct bmx280_calib *c, int32_t t_fine, int32_t adc_p);
// %RH in Q22.10 (DS002 4.2.3).
uint32_t bmx280_humidity(const struct bmx280_calib *c, int32_t t_fine, int32_t adc_h);

// The inverses. Each returns the raw word whose compensated value is
// nearest the target; the temperature one also gives that word's t_fine,
// which the other two take.
uint32_t bmx280_raw_temperature(const struct bmx280_calib *c, int32_t centi_c, int32_t *t_fine);
// Of the words the 32-bit formula maps nearest `pa`, the one the 64-bit
// formula puts nearest it too. The two formulas disagree by up to a few
// pascals (test/unit/bmx280_check.c), so it is the 32-bit one that is exact.
uint32_t bmx280_raw_pressure(const struct bmx280_calib *c, int32_t t_fine, int32_t pa);
uint32_t bmx280_raw_humidity(const struct bmx280_calib *c, int32_t t_fine, uint32_t q10);

// Rounded to the nearest of the units the formulas produce.
int32_t bmx280_centi_c(int32_t milli_c);
uint32_t bmx280_humidity_q10(int32_t milli_rh);

#endif
