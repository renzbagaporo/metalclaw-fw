// SPDX-License-Identifier: Apache-2.0 OR MIT
// TI TMP108 (SBOS663A), TMP112 (SBOS473) and TMP116 (SNOSD68): pointers and
// the configuration bits the models use.

#ifndef MCLW_DEVICE_TMP1XX_H
#define MCLW_DEVICE_TMP1XX_H

enum tmp1xx_kind {
    TMP1XX_TMP108,
    TMP1XX_TMP112,
    TMP1XX_TMP116,
};

// TMP108 table 4, TMP112's pointer register: temperature, configuration,
// TLOW, THIGH.
#define TMP1XX_PTR_TEMP 0x00
#define TMP1XX_PTR_CONFIG 0x01
#define TMP1XX_PTR_TLOW 0x02
#define TMP1XX_PTR_THIGH 0x03
// TMP116's register map: THIGH before TLOW, the EEPROM, the device ID.
#define TMP116_PTR_THIGH 0x02
#define TMP116_PTR_TLOW 0x03
#define TMP116_PTR_EEPROM_UL 0x04
#define TMP116_PTR_EEPROM1 0x05
#define TMP116_PTR_EEPROM2 0x06
#define TMP116_PTR_EEPROM3 0x07
#define TMP116_PTR_EEPROM4 0x08
#define TMP116_PTR_DEVICE_ID 0x0f

// TMP108 configuration (table 8): ID CR1 CR0 FH FL TM M1 M0 | POL 0 HYS1 HYS0.
#define TMP108_FH 0x1000u
#define TMP108_FL 0x0800u
#define TMP108_POL 0x0080u
// TMP112 configuration: OS R1 R0 F1 F0 POL TM SD | CR1 CR0 AL EM.
#define TMP112_OS 0x8000u
#define TMP112_POL 0x0400u
#define TMP112_SD 0x0100u
#define TMP112_AL 0x0020u
#define TMP112_EM 0x0010u
// TMP116 configuration: HIGH_Alert LOW_Alert Data_Ready EEPROM_Busy MOD[1:0]
// CONV[2:0] AVG[1:0] T/nA POL DR/Alert.
#define TMP116_HIGH_ALERT 0x8000u
#define TMP116_LOW_ALERT 0x4000u
#define TMP116_DATA_READY 0x2000u
#define TMP116_POL 0x0008u

#endif
