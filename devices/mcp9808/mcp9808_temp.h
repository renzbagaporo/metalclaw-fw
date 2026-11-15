// SPDX-License-Identifier: Apache-2.0 OR MIT
// The MCP9808's temperature words (DS25095A registers 5-3 and 5-4), apart
// from the model so the same source is checked on the host
// (test/unit/test_part_models.py).

#ifndef MCLW_DEVICE_MCP9808_TEMP_H
#define MCLW_DEVICE_MCP9808_TEMP_H

#include <stdint.h>

// TUPPER, TLOWER, TCRIT: sign at bit 12, 0.25 degC at bit 2 (register 5-3).
#define MCP9808_LIMIT_MASK 0x1ffcu
// TA: TA >= TCRIT, TA > TUPPER, TA < TLOWER at bits 15..13, then 13 bits of
// two's complement in 0.0625 degC (register 5-4).
#define MCP9808_TA_CRIT 0x8000u
#define MCP9808_TA_UPPER 0x4000u
#define MCP9808_TA_LOWER 0x2000u
#define MCP9808_TA_VALUE_MASK 0x1fffu

// Sixteenths of a degree, rounded to the nearest, within the 13-bit range.
int32_t mcp9808_sixteenths(int32_t milli_c);
// A limit register's temperature, in sixteenths.
int32_t mcp9808_limit_sixteenths(uint16_t limit);
int32_t mcp9808_limit_milli_c(uint16_t limit);
// The TA word for a temperature at a resolution setting (register 5-7:
// 0 is 0.5 degC .. 3 is 0.0625 degC; the low bits a coarser setting does
// not resolve read as zero) with its comparison bits against the limits.
uint16_t mcp9808_ta_word(int32_t milli_c, uint8_t resolution, uint16_t upper, uint16_t lower,
    uint16_t crit);

#endif
