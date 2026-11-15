// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_PCF8523_H
#define MCLW_DEVICE_PCF8523_H

#include "metalclaw/model.h"

#include "pcf8523_clock.h"

// Register addresses (data sheet Rev. 7, table 6).
#define PCF8523_REG_CONTROL_1 0x00
#define PCF8523_REG_CONTROL_2 0x01
#define PCF8523_REG_CONTROL_3 0x02
#define PCF8523_REG_SECONDS 0x03
#define PCF8523_REG_YEARS 0x09
#define PCF8523_REG_MINUTE_ALARM 0x0a
#define PCF8523_REG_WEEKDAY_ALARM 0x0d
#define PCF8523_REG_OFFSET 0x0e
#define PCF8523_REG_TMR_CLKOUT_CTRL 0x0f
#define PCF8523_REG_TMR_A_FREQ_CTRL 0x10
#define PCF8523_REG_TMR_A_REG 0x11
#define PCF8523_REG_TMR_B_FREQ_CTRL 0x12
#define PCF8523_REG_TMR_B_REG 0x13
#define PCF8523_REGISTERS 0x14

// Control_1 (table 7).
#define PCF8523_CONTROL_1_STOP 0x20
#define PCF8523_CONTROL_1_SR 0x10
#define PCF8523_CONTROL_1_12_24 0x08
// 8.3: the software reset command is exactly this value in Control_1.
#define PCF8523_SOFTWARE_RESET 0x58
// Control_2 (table 8): WTAF is read-only, CTAF, CTBF, SF and AF clear on a
// written 0 and keep on a written 1 (8.7.5), the interrupt enables are plain.
#define PCF8523_CONTROL_2_WTAF 0x80
#define PCF8523_CONTROL_2_FLAGS 0x78
#define PCF8523_CONTROL_2_AF 0x08
#define PCF8523_CONTROL_2_ENABLES 0x07
// Control_3 (table 9): PM and the two interrupt enables; BSF and BLF are
// flags the model never raises.
#define PCF8523_CONTROL_3_WRITABLE 0xe3

// Table 10.
#define PCF8523_CONTROL_3_RESET 0xe0
#define PCF8523_TMR_FREQ_CTRL_RESET 0x07

// After STOP is released the first second takes 0.5 s (8.10, table 45).
#define PCF8523_SECOND_US 1000000u
#define PCF8523_STOP_RELEASE_US 500000u

extern const struct mclw_model mclw_model_pcf8523;

#endif
