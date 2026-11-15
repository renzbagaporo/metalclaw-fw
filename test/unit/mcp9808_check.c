// SPDX-License-Identifier: Apache-2.0 OR MIT
// Checks devices/mcp9808/mcp9808_temp.c on the host: the TA word's value,
// resolution and comparison bits (DS25095A registers 5-3, 5-4 and 5-7), and
// what pico-examples' mcp9808_i2c makes of it.
//
//   mcp9808_check      checks; prints "ok"
//   mcp9808_check MC   prints what mcp9808_i2c prints for one reading, with
//                      the limits it writes (30, 20 and 40 degC)

#include <stdio.h>
#include <stdlib.h>

#include "mcp9808_temp.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("failed: %s (line %d)\n", #cond, __LINE__); \
            exit(1); \
        } \
    } while (0)

// mcp9808_set_limits() in mcp9808_i2c.c, MSB first.
#define UPPER 0x01e0
#define LOWER 0x0140
#define CRIT 0x0280

static int32_t value_milli_c(uint16_t word) {
    int32_t t = (int32_t)(word & MCP9808_TA_VALUE_MASK);
    return (t >= 0x1000 ? t - 0x2000 : t) * 125 / 2;
}

int main(int argc, char **argv) {
    if (argc == 2) {
        uint16_t word = mcp9808_ta_word(atoi(argv[1]), 3, UPPER, LOWER, CRIT);
        uint8_t upper_byte = (uint8_t)(word >> 8);
        uint8_t lower_byte = (uint8_t)word;
        float temperature;

        // mcp9808_check_limits(upper_byte & 0xE0), then
        // mcp9808_convert_temp(upper_byte & 0x1F, lower_byte).
        if ((upper_byte & 0x40) == 0x40) {
            printf("Temperature is above the upper temperature limit.\n");
        }
        if ((upper_byte & 0x20) == 0x20) {
            printf("Temperature is below the lower temperature limit.\n");
        }
        if ((upper_byte & 0x80) == 0x80) {
            printf("Temperature is above the critical temperature limit.\n");
        }
        upper_byte &= 0x1f;
        if ((upper_byte & 0x10) == 0x10) {
            upper_byte = upper_byte & 0x0F;
            temperature = 256 - (((float)upper_byte * 16) + ((float)lower_byte / 16));
        } else {
            temperature = (((float)upper_byte * 16) + ((float)lower_byte / 16));
        }
        printf("Ambient temperature: %.4f\xc2\xb0" "C\n", (double)temperature);
        return 0;
    }

    // Nearest sixteenth over the property's range, at full resolution.
    for (int32_t mc = -40000; mc <= 125000; mc++) {
        int32_t back = value_milli_c(mcp9808_ta_word(mc, 3, 0, 0, 0));
        CHECK(back - mc <= 32 && mc - back <= 32);
    }
    // DS25095A figure 5-5: 0x0194 reads +25.25 degC.
    CHECK((mcp9808_ta_word(25250, 3, 0x0fff, 0x1000, 0x0fff) & MCP9808_TA_VALUE_MASK) == 0x0194);
    // Coarser settings clear the bits they do not resolve, flooring.
    CHECK((mcp9808_ta_word(25300, 0, 0, 0, 0) & 0x0007) == 0);
    CHECK(value_milli_c(mcp9808_ta_word(25300, 0, 0, 0, 0)) == 25000);
    CHECK(value_milli_c(mcp9808_ta_word(-300, 0, 0, 0, 0)) == -500);
    CHECK(value_milli_c(mcp9808_ta_word(25300, 1, 0, 0, 0)) == 25250);
    CHECK(value_milli_c(mcp9808_ta_word(25200, 2, 0, 0, 0)) == 25125);
    // Limits: 0.25 degC, sign at bit 12; the low two bits and the top three
    // are not part of them.
    CHECK(mcp9808_limit_milli_c(UPPER) == 30000);
    CHECK(mcp9808_limit_milli_c(LOWER) == 20000);
    CHECK(mcp9808_limit_milli_c(CRIT) == 40000);
    CHECK(mcp9808_limit_milli_c(0x1ffc) == -250);
    CHECK(mcp9808_limit_milli_c(0xe003) == 0);
    // The comparison bits: TA >= TCRIT, TA > TUPPER, TA < TLOWER.
    CHECK((mcp9808_ta_word(25000, 3, UPPER, LOWER, CRIT) & 0xe000) == 0);
    CHECK((mcp9808_ta_word(30000, 3, UPPER, LOWER, CRIT) & 0xe000) == 0);
    CHECK((mcp9808_ta_word(30063, 3, UPPER, LOWER, CRIT) & 0xe000) == MCP9808_TA_UPPER);
    CHECK((mcp9808_ta_word(40000, 3, UPPER, LOWER, CRIT) & 0xe000) == (MCP9808_TA_CRIT | MCP9808_TA_UPPER));
    CHECK((mcp9808_ta_word(19937, 3, UPPER, LOWER, CRIT) & 0xe000) == MCP9808_TA_LOWER);
    CHECK((mcp9808_ta_word(-5000, 3, 0x1fa0, 0x1f60, 0x1fd8) & 0xe000) == MCP9808_TA_UPPER);
    printf("ok\n");
    return 0;
}
