// SPDX-License-Identifier: Apache-2.0 OR MIT
// Checks devices/pcf8523/pcf8523_clock.c on the host: BCD, the counters'
// carries through months, leap years and the century, the 12 hour coding,
// and the alarm flag on the minute increment that first matches.
//
//   pcf8523_check        checks; prints "ok"
//   pcf8523_check S      prints what pico-examples' pcf8523_i2c prints S
//                        seconds after it set its time and alarm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcf8523_clock.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("failed: %s (line %d)\n", #cond, __LINE__); \
            exit(1); \
        } \
    } while (0)

static void set(uint8_t regs[PCF8523_TIME_REGS], uint8_t y, uint8_t mo, uint8_t d, uint8_t wd,
    uint8_t h, uint8_t mi, uint8_t s, bool twelve) {
    struct pcf8523_time t = { .year = y, .month = mo, .day = d, .weekday = wd, .hour = h,
        .minute = mi, .second = s };
    pcf8523_encode(&t, twelve, regs);
}

static bool is(const uint8_t regs[PCF8523_TIME_REGS], uint8_t y, uint8_t mo, uint8_t d, uint8_t wd,
    uint8_t h, uint8_t mi, uint8_t s, bool twelve) {
    struct pcf8523_time t;
    pcf8523_decode(regs, twelve, &t);
    return t.year == y && t.month == mo && t.day == d && t.weekday == wd && t.hour == h
        && t.minute == mi && t.second == s;
}

// pcf8520_convert_time() and the loop's two printf lines, then
// pcf8520_check_alarm().
static void print_example(const uint8_t raw[PCF8523_TIME_REGS], uint8_t control_2) {
    static const char days[7][12] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
        "Friday", "Saturday" };
    int t[7];
    t[0] = (10 * ((raw[0] & 0x70) >> 4)) + (raw[0] & 0x0F);
    t[1] = (10 * ((raw[1] & 0x70) >> 4)) + (raw[1] & 0x0F);
    t[2] = (10 * ((raw[2] & 0x30) >> 4)) + (raw[2] & 0x0F);
    t[3] = (10 * ((raw[3] & 0x30) >> 4)) + (raw[3] & 0x0F);
    t[4] = raw[4] & 0x07;
    t[5] = (10 * ((raw[5] & 0x10) >> 4)) + (raw[5] & 0x0F);
    t[6] = (10 * ((raw[6] & 0xF0) >> 4)) + (raw[6] & 0x0F);
    printf("Time: %02d : %02d : %02d\n", t[2], t[1], t[0]);
    printf("Date: %s %02d / %02d / %02d\n", days[t[4]], t[3], t[5], t[6]);
    printf("%s\n", (control_2 & 0x08) == 0x08 ? "ALARM RINGING" : "Alarm not triggered yet");
}

int main(int argc, char **argv) {
    uint8_t regs[PCF8523_TIME_REGS];
    const uint8_t off[PCF8523_ALARM_REGS] = { 0x80, 0x80, 0x80, 0x80 };

    if (argc == 2) {
        // pcf820_write_current_time() writes 0 to 03h..09h and
        // pcf8520_set_alarm() 01h, 80h, 80h, 80h to 0Ah..0Dh.
        const uint8_t alarm[PCF8523_ALARM_REGS] = { 0x01, 0x80, 0x80, 0x80 };
        uint8_t control_2 = 0;
        memset(regs, 0, sizeof(regs));
        for (long s = atol(argv[1]); s > 0; s--) {
            if (pcf8523_advance(regs, false, 1, alarm)) {
                control_2 |= 0x08;
            }
        }
        print_example(regs, control_2);
        return 0;
    }

    for (uint8_t v = 0; v < 100; v++) {
        CHECK(pcf8523_from_bcd(pcf8523_to_bcd(v)) == v);
    }
    CHECK(pcf8523_to_bcd(59) == 0x59);

    // Seconds into minutes, hours, days; the weekday follows the day.
    set(regs, 26, 11, 15, 0, 23, 59, 58, false);
    CHECK(!pcf8523_advance(regs, false, 1, off));
    CHECK(is(regs, 26, 11, 15, 0, 23, 59, 59, false));
    pcf8523_advance(regs, false, 1, off);
    CHECK(is(regs, 26, 11, 16, 1, 0, 0, 0, false));
    // Month ends, February in a leap year and not, the century.
    set(regs, 26, 11, 30, 1, 23, 59, 59, false);
    pcf8523_advance(regs, false, 1, off);
    CHECK(is(regs, 26, 12, 1, 2, 0, 0, 0, false));
    set(regs, 24, 2, 28, 3, 23, 59, 59, false);
    pcf8523_advance(regs, false, 1, off);
    CHECK(is(regs, 24, 2, 29, 4, 0, 0, 0, false));
    set(regs, 23, 2, 28, 2, 23, 59, 59, false);
    pcf8523_advance(regs, false, 1, off);
    CHECK(is(regs, 23, 3, 1, 3, 0, 0, 0, false));
    set(regs, 99, 12, 31, 6, 23, 59, 59, false);
    pcf8523_advance(regs, false, 1, off);
    CHECK(is(regs, 0, 1, 1, 0, 0, 0, 0, false));
    // A long step equals as many single ones, and keeps OS.
    set(regs, 26, 11, 15, 0, 12, 34, 56, false);
    regs[0] |= PCF8523_SECONDS_OS;
    pcf8523_advance(regs, false, 86400u * 400u + 3725u, off);
    CHECK(is(regs, 27, 12, 20, 1, 13, 37, 1, false));
    CHECK((regs[0] & PCF8523_SECONDS_OS) != 0);

    // 12 hour coding (table 15): 1..12 with AMPM.
    set(regs, 0, 1, 1, 6, 0, 0, 0, true);
    CHECK(regs[2] == 0x12);
    set(regs, 0, 1, 1, 6, 12, 0, 0, true);
    CHECK(regs[2] == 0x32);
    set(regs, 0, 1, 1, 6, 13, 0, 0, true);
    CHECK(regs[2] == 0x21);
    set(regs, 0, 1, 1, 6, 11, 59, 59, true);
    pcf8523_advance(regs, true, 1, off);
    CHECK(regs[2] == 0x32 && is(regs, 0, 1, 1, 6, 12, 0, 0, true));

    // The alarm: no field enabled never matches; AF on the increment that
    // first matches, not again while it still matches, and again the next
    // time it comes round.
    CHECK(!pcf8523_alarm_match(regs, off));
    {
        const uint8_t alarm[PCF8523_ALARM_REGS] = { 0x01, 0x80, 0x80, 0x80 };
        memset(regs, 0, sizeof(regs));
        CHECK(!pcf8523_advance(regs, false, 59, alarm));
        CHECK(pcf8523_advance(regs, false, 1, alarm));
        CHECK(regs[1] == 0x01 && regs[0] == 0x00);
        CHECK(!pcf8523_advance(regs, false, 59, alarm));
        CHECK(!pcf8523_advance(regs, false, 3540, alarm));
        CHECK(regs[2] == 0x01 && regs[1] == 0x00);
        CHECK(pcf8523_advance(regs, false, 60, alarm));
        // A step that passes a match flags it.
        memset(regs, 0, sizeof(regs));
        CHECK(pcf8523_advance(regs, false, 7200, alarm));
    }
    {
        // Hour and minute together, 12 hour clock: 2:30 PM.
        const uint8_t alarm[PCF8523_ALARM_REGS] = { 0x30, 0x22, 0x80, 0x80 };
        set(regs, 0, 1, 1, 6, 2, 29, 0, true);
        CHECK(!pcf8523_advance(regs, true, 60, alarm));
        set(regs, 0, 1, 1, 6, 14, 29, 0, true);
        CHECK(pcf8523_advance(regs, true, 60, alarm));
    }
    printf("ok\n");
    return 0;
}
