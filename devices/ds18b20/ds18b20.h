// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_DS18B20_H
#define MCLW_DEVICE_DS18B20_H

#include <stdint.h>

#include "metalclaw/model.h"

// Maxim DS18B20 data sheet REV 042208. Function commands, Table 3.
#define DS18B20_CONVERT_T 0x44u
#define DS18B20_WRITE_SCRATCHPAD 0x4eu
#define DS18B20_READ_SCRATCHPAD 0xbeu
#define DS18B20_COPY_SCRATCHPAD 0x48u
#define DS18B20_RECALL_E2 0xb8u
#define DS18B20_READ_POWER_SUPPLY 0xb4u

#define DS18B20_FAMILY 0x28u

// Scratchpad bytes (Figure 7).
#define DS18B20_SP_TEMP_LSB 0u
#define DS18B20_SP_TEMP_MSB 1u
#define DS18B20_SP_TH 2u
#define DS18B20_SP_TL 3u
#define DS18B20_SP_CONFIG 4u
#define DS18B20_SP_RESERVED 5u
#define DS18B20_SP_CRC 8u
#define DS18B20_SCRATCHPAD_BYTES 9u

// Configuration register (Figure 8): R1 and R0 are bits 6 and 5; bit 7
// reads 0 and bits 4..0 read 1, and none of them can be written.
#define DS18B20_CONFIG_WRITABLE 0x60u
#define DS18B20_CONFIG_FIXED 0x1fu
// R1 = R0 = 1, 12 bits, the power-up default the text gives (Table 2).
#define DS18B20_CONFIG_DEFAULT 0x7fu
// The data sheet leaves TH and TL to the user's EEPROM; the model starts
// them at the measuring range's ends, so only those ends raise the alarm.
#define DS18B20_TH_DEFAULT 125
#define DS18B20_TL_DEFAULT (-55)

#define DS18B20_TEMPERATURE_MIN_MC (-55000)
#define DS18B20_TEMPERATURE_MAX_MC 125000
#define DS18B20_TEMPERATURE_DEFAULT_MC 25000
#define DS18B20_SERIAL_DEFAULT 0x00b20b20u

uint32_t ds18b20_resolution_bits(uint8_t config);
uint32_t ds18b20_conversion_us(uint8_t config);
int16_t ds18b20_temperature_word(int32_t milli_c, uint8_t config);

extern const struct mclw_model mclw_model_ds18b20;

#endif
