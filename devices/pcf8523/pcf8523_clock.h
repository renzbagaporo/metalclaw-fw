// SPDX-License-Identifier: Apache-2.0 OR MIT
// The PCF8523's time counters (data sheet Rev. 7, 8.6) and alarm match
// (8.7.5) on the registers' own BCD, apart from the model so the same
// source is checked on the host (test/unit/test_part_models.py).

#ifndef MCLW_DEVICE_PCF8523_CLOCK_H
#define MCLW_DEVICE_PCF8523_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

// Seconds through Years, 03h..09h, and Minute_alarm through Weekday_alarm,
// 0Ah..0Dh, as the registers hold them.
#define PCF8523_TIME_REGS 7
#define PCF8523_ALARM_REGS 4

#define PCF8523_SECONDS_OS 0x80
#define PCF8523_HOURS_PM 0x20
#define PCF8523_ALARM_AEN 0x80

// Decimal fields; hour is 0..23 whichever mode the Hours register is in.
struct pcf8523_time {
    uint8_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

uint8_t pcf8523_to_bcd(uint8_t value);
uint8_t pcf8523_from_bcd(uint8_t bcd);
// Leap years are those the year counter divides by 4, 00 included (8.6.4).
uint8_t pcf8523_days_in_month(uint8_t month, uint8_t year);
void pcf8523_decode(const uint8_t regs[PCF8523_TIME_REGS], bool twelve_hour, struct pcf8523_time *t);
// Seconds' OS bit is left clear.
void pcf8523_encode(const struct pcf8523_time *t, bool twelve_hour, uint8_t regs[PCF8523_TIME_REGS]);
// Every enabled alarm field equals its counter, and at least one is enabled.
bool pcf8523_alarm_match(const uint8_t regs[PCF8523_TIME_REGS], const uint8_t alarm[PCF8523_ALARM_REGS]);
// Counts `seconds` 1 Hz ticks into the registers, OS bit kept. True if a
// minute increment brought the enabled alarms to a match they did not
// have before it: the moment AF is set (8.7.5).
bool pcf8523_advance(uint8_t regs[PCF8523_TIME_REGS], bool twelve_hour, uint32_t seconds,
    const uint8_t alarm[PCF8523_ALARM_REGS]);

#endif
