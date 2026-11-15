// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdint.h>

#include "metalclaw/hal/platform.h"

#include "mcp9808_temp.h"

#define SIXTEENTHS_MIN (-4096)
#define SIXTEENTHS_MAX 4095

int32_t MCLW_RAM_FUNC(mcp9808_sixteenths)(int32_t milli_c) {
    int32_t scaled = milli_c * 2;
    int32_t t = scaled >= 0 ? (scaled + 62) / 125 : -((62 - scaled) / 125);

    if (t < SIXTEENTHS_MIN) {
        return SIXTEENTHS_MIN;
    }
    return t > SIXTEENTHS_MAX ? SIXTEENTHS_MAX : t;
}

int32_t MCLW_RAM_FUNC(mcp9808_limit_sixteenths)(uint16_t limit) {
    int32_t value = (int32_t)(limit & MCP9808_LIMIT_MASK);

    return value >= 0x1000 ? value - 0x2000 : value;
}

int32_t mcp9808_limit_milli_c(uint16_t limit) {
    return mcp9808_limit_sixteenths(limit) * 125 / 2;
}

uint16_t MCLW_RAM_FUNC(mcp9808_ta_word)(int32_t milli_c, uint8_t resolution, uint16_t upper,
    uint16_t lower, uint16_t crit) {
    int32_t t = mcp9808_sixteenths(milli_c);
    uint16_t word;

    // Two's complement, so clearing the unresolved bits floors.
    t = (int32_t)((uint32_t)t & ~((1u << (3u - (resolution & 3u))) - 1u));
    word = (uint16_t)((uint32_t)t & MCP9808_TA_VALUE_MASK);
    if (t >= mcp9808_limit_sixteenths(crit)) {
        word |= MCP9808_TA_CRIT;
    }
    if (t > mcp9808_limit_sixteenths(upper)) {
        word |= MCP9808_TA_UPPER;
    }
    if (t < mcp9808_limit_sixteenths(lower)) {
        word |= MCP9808_TA_LOWER;
    }
    return word;
}
