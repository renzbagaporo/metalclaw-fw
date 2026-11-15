// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_BMX280_H
#define MCLW_DEVICE_BMX280_H

#include "metalclaw/model.h"

#include "bmx280_comp.h"

// Register addresses as the datasheets give them (BST-BMP280-DS001 table 18,
// BST-BME280-DS002 table 18). Over SPI a part drops bit 7 (DS001 5.3,
// DS002 6.3), so the model's image holds each register at its address with
// bit 7 cleared when the SPI binding is the one bound.
#define BMX280_REG_CALIB_00 0x88
#define BMX280_REG_CALIB_25 0xa1
#define BMX280_REG_ID 0xd0
#define BMX280_REG_RESET 0xe0
#define BMX280_REG_CALIB_26 0xe1
#define BMX280_REG_CALIB_41 0xf0
#define BMX280_REG_CTRL_HUM 0xf2
#define BMX280_REG_STATUS 0xf3
#define BMX280_REG_CTRL_MEAS 0xf4
#define BMX280_REG_CONFIG 0xf5
#define BMX280_REG_PRESS_MSB 0xf7
#define BMX280_REG_TEMP_MSB 0xfa
#define BMX280_REG_HUM_MSB 0xfd

#define BMP280_CHIP_ID 0x58
#define BME280_CHIP_ID 0x60
#define BMX280_RESET_WORD 0xb6

#define BMX280_STATUS_MEASURING 0x08
#define BMX280_MODE_MASK 0x03
#define BMX280_MODE_SLEEP 0x00
#define BMX280_MODE_NORMAL 0x03
#define BMX280_OSRS_T_POS 5
#define BMX280_OSRS_P_POS 2
#define BMX280_OSRS_MASK 0x07
#define BMX280_CONFIG_WRITABLE 0xfd
#define BMX280_CTRL_HUM_WRITABLE 0x07

// What a skipped measurement reads (DS001 tables 4 and 5, DS002 table 20).
#define BMX280_SKIPPED_20 0x80000u
#define BMX280_SKIPPED_16 0x8000u

#define BMX280_PRESSURE_MIN_PA 30000
#define BMX280_PRESSURE_MAX_PA 110000
#define BMX280_PRESSURE_DEFAULT_PA 101325
#define BMX280_TEMPERATURE_MIN_MC (-40000)
#define BMX280_TEMPERATURE_MAX_MC 85000
#define BMX280_TEMPERATURE_DEFAULT_MC 25000
#define BME280_HUMIDITY_MIN_MRH 0
#define BME280_HUMIDITY_MAX_MRH 100000
#define BME280_HUMIDITY_DEFAULT_MRH 50000

extern const struct mclw_model mclw_model_bmp280;
extern const struct mclw_model mclw_model_bme280;

#endif
