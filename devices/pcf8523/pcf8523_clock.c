// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdbool.h>
#include <stdint.h>

#include "pcf8523_clock.h"

enum {
    SECONDS,
    MINUTES,
    HOURS,
    DAYS,
    WEEKDAYS,
    MONTHS,
    YEARS,
};

uint8_t pcf8523_to_bcd(uint8_t value) {
    return (uint8_t)(((value / 10u) << 4) | (value % 10u));
}

// A register may hold what is not BCD (a driver wrote it); it decodes to
// whatever the digits add up to, and the counters carry it on from there.
uint8_t pcf8523_from_bcd(uint8_t bcd) {
    return (uint8_t)((bcd >> 4) * 10u + (bcd & 0x0fu));
}

uint8_t pcf8523_days_in_month(uint8_t month, uint8_t year) {
    switch (month) {
        case 2:
            return year % 4u == 0 ? 29 : 28;
        case 4:
        case 6:
        case 9:
        case 11:
            return 30;
        default:
            return 31;
    }
}

void pcf8523_decode(const uint8_t regs[PCF8523_TIME_REGS], bool twelve_hour, struct pcf8523_time *t) {
    t->second = pcf8523_from_bcd(regs[SECONDS] & 0x7fu);
    t->minute = pcf8523_from_bcd(regs[MINUTES] & 0x7fu);
    if (twelve_hour) {
        t->hour = (uint8_t)(pcf8523_from_bcd(regs[HOURS] & 0x1fu) % 12u
            + ((regs[HOURS] & PCF8523_HOURS_PM) != 0 ? 12u : 0u));
    } else {
        t->hour = pcf8523_from_bcd(regs[HOURS] & 0x3fu);
    }
    t->day = pcf8523_from_bcd(regs[DAYS] & 0x3fu);
    t->weekday = regs[WEEKDAYS] & 0x07u;
    t->month = pcf8523_from_bcd(regs[MONTHS] & 0x1fu);
    t->year = pcf8523_from_bcd(regs[YEARS]);
}

void pcf8523_encode(const struct pcf8523_time *t, bool twelve_hour, uint8_t regs[PCF8523_TIME_REGS]) {
    regs[SECONDS] = pcf8523_to_bcd(t->second);
    regs[MINUTES] = pcf8523_to_bcd(t->minute);
    if (twelve_hour) {
        uint8_t h12 = t->hour % 12u == 0 ? 12u : t->hour % 12u;
        regs[HOURS] = (uint8_t)(pcf8523_to_bcd(h12) | (t->hour >= 12u ? PCF8523_HOURS_PM : 0u));
    } else {
        regs[HOURS] = pcf8523_to_bcd(t->hour);
    }
    regs[DAYS] = pcf8523_to_bcd(t->day);
    regs[WEEKDAYS] = t->weekday & 0x07u;
    regs[MONTHS] = pcf8523_to_bcd(t->month);
    regs[YEARS] = pcf8523_to_bcd(t->year);
}

// The alarm registers compare with the counters' own encoding, so a 12 hour
// clock compares its AMPM bit too (tables 22 to 25).
bool pcf8523_alarm_match(const uint8_t regs[PCF8523_TIME_REGS], const uint8_t alarm[PCF8523_ALARM_REGS]) {
    static const uint8_t field[PCF8523_ALARM_REGS] = { MINUTES, HOURS, DAYS, WEEKDAYS };
    static const uint8_t mask[PCF8523_ALARM_REGS] = { 0x7f, 0x3f, 0x3f, 0x07 };
    bool enabled = false;

    for (int i = 0; i < PCF8523_ALARM_REGS; i++) {
        if ((alarm[i] & PCF8523_ALARM_AEN) != 0) {
            continue;
        }
        enabled = true;
        if ((alarm[i] & mask[i]) != (regs[field[i]] & mask[i])) {
            return false;
        }
    }
    return enabled;
}

static void next_day(struct pcf8523_time *t) {
    t->weekday = t->weekday >= 6u ? 0u : (uint8_t)(t->weekday + 1u);
    if (t->day >= pcf8523_days_in_month(t->month, t->year)) {
        t->day = 1;
        if (t->month >= 12u) {
            t->month = 1;
            t->year = t->year >= 99u ? 0u : (uint8_t)(t->year + 1u);
        } else {
            t->month++;
        }
    } else {
        t->day++;
    }
}

static void next_minute(struct pcf8523_time *t) {
    if (t->minute < 59u) {
        t->minute++;
        return;
    }
    t->minute = 0;
    if (t->hour < 23u) {
        t->hour++;
        return;
    }
    t->hour = 0;
    next_day(t);
}

bool pcf8523_advance(uint8_t regs[PCF8523_TIME_REGS], bool twelve_hour, uint32_t seconds,
    const uint8_t alarm[PCF8523_ALARM_REGS]) {
    uint8_t os = regs[SECONDS] & PCF8523_SECONDS_OS;
    struct pcf8523_time t;
    bool flagged = false;

    pcf8523_decode(regs, twelve_hour, &t);
    while (seconds > 0) {
        uint32_t to_minute = t.second < 60u ? 60u - t.second : 1u;
        bool before;

        if (seconds < to_minute) {
            t.second = (uint8_t)(t.second + seconds);
            break;
        }
        seconds -= to_minute;
        t.second = 0;
        before = pcf8523_alarm_match(regs, alarm);
        next_minute(&t);
        pcf8523_encode(&t, twelve_hour, regs);
        if (!before && pcf8523_alarm_match(regs, alarm)) {
            flagged = true;
        }
    }
    pcf8523_encode(&t, twelve_hour, regs);
    regs[SECONDS] |= os;
    return flagged;
}
