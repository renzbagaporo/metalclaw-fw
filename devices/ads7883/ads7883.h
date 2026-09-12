// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef METALCLAW_DEVICE_ADS7883_H
#define METALCLAW_DEVICE_ADS7883_H

#include <stdint.h>

#include "metalclaw/model.h"

#define ADS7883_SAMPLE_MAX 0x0fff
#define ADS7883_SAMPLE_DEFAULT 0
#define ADS7883_FRAME_BYTES 2

// 16 clocks: two leading zeros, the 12-bit result MSB first, two trailing
// zeros. SDO stays low after the sixteenth clock.
uint16_t metalclaw_ads7883_frame(uint16_t sample);

extern const struct metalclaw_model metalclaw_model_ads7883;

#endif
