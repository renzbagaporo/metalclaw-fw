// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_WS2812B_H
#define MCLW_DEVICE_WS2812B_H

#include "metalclaw/model.h"

#define WS2812B_LEDS_MAX 256u
#define WS2812B_BYTES_MAX (WS2812B_LEDS_MAX * 4u)

extern const struct mclw_model mclw_model_ws2812b;

#endif
