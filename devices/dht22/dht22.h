// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_DHT22_H
#define MCLW_DEVICE_DHT22_H

#include <stdint.h>

#include "metalclaw/model.h"

// Aosong DHT22 (AM2302) data sheet, section 6(2), steps 1 to 3. Where it
// gives a range the model takes the middle: 20-40 us, 26-28 us.
#define DHT22_WAIT_NS 30000u
#define DHT22_RESPONSE_NS 80000u
#define DHT22_BIT_LOW_NS 50000u
#define DHT22_ZERO_NS 27000u
#define DHT22_ONE_NS 70000u

#define DHT22_FRAME_BYTES 5u
// The wait, the response's low and high, a low and a high for each bit,
// and the closing low.
#define DHT22_PULSES (3u + DHT22_FRAME_BYTES * 8u * 2u + 1u)

// Section 3: -40 to 80 degC.
#define DHT22_TEMPERATURE_MIN_MC (-40000)
#define DHT22_TEMPERATURE_MAX_MC 80000
#define DHT22_TEMPERATURE_DEFAULT_MC 25000
#define DHT22_HUMIDITY_DEFAULT_MPCT 50000u

void dht22_frame(uint32_t humidity_mpct, int32_t temperature_mc, uint8_t frame[DHT22_FRAME_BYTES]);
void dht22_waveform(const uint8_t frame[DHT22_FRAME_BYTES], uint32_t pulses[DHT22_PULSES]);

extern const struct mclw_model mclw_model_dht22;

#endif
