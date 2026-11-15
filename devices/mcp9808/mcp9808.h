// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_MCP9808_H
#define MCLW_DEVICE_MCP9808_H

#include "metalclaw/model.h"

#include "mcp9808_temp.h"

// Register pointers (DS25095A register 5-1). Registers are 16 bits, MSB
// first, except Resolution, which is one byte; the image holds register p
// at bytes 2p and 2p + 1, and the pointer map sends pointer p to byte 2p.
#define MCP9808_REG_RFU 0x0
#define MCP9808_REG_CONFIG 0x1
#define MCP9808_REG_UPPER 0x2
#define MCP9808_REG_LOWER 0x3
#define MCP9808_REG_CRIT 0x4
#define MCP9808_REG_TA 0x5
#define MCP9808_REG_MANUFACTURER 0x6
#define MCP9808_REG_DEVICE 0x7
#define MCP9808_REG_RESOLUTION 0x8
#define MCP9808_REGISTER_BYTES 2

// Power-on values (DS25095A table 5-1 and table 5-3).
#define MCP9808_RFU_VALUE 0x001f
#define MCP9808_MANUFACTURER_ID 0x0054
#define MCP9808_DEVICE_ID 0x0400
#define MCP9808_RESOLUTION_RESET 0x03

// CONFIG (register 5-2): hysteresis and SHDN in the MSB; in the LSB the two
// locks, Int Clr (reads 0) and Alert Stat (the Alert pin, not modelled).
#define MCP9808_CONFIG_MSB_MASK 0x07
#define MCP9808_CONFIG_SHDN 0x01
#define MCP9808_CONFIG_LSB_MASK 0xcf
#define MCP9808_CONFIG_CRIT_LOCK 0x80
#define MCP9808_CONFIG_WIN_LOCK 0x40
#define MCP9808_RESOLUTION_MASK 0x03

#define MCP9808_TEMPERATURE_MIN_MC (-40000)
#define MCP9808_TEMPERATURE_MAX_MC 125000
#define MCP9808_TEMPERATURE_DEFAULT_MC 25000

extern const struct mclw_model mclw_model_mcp9808;

#endif
