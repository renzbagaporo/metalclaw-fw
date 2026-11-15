// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_ST7789V_H
#define MCLW_DEVICE_ST7789V_H

#include "metalclaw/model.h"

// Every eighth column and row of the 240 x 320 frame memory, RGB 5-6-5,
// big-endian: pixel (8x, 8y) at bytes 2 (30y + x) and the one after.
#define ST7789V_THUMBNAIL_COLUMNS 30u
#define ST7789V_THUMBNAIL_ROWS 40u
#define ST7789V_THUMBNAIL_BYTES (2u * ST7789V_THUMBNAIL_COLUMNS * ST7789V_THUMBNAIL_ROWS)

extern const struct mclw_model mclw_model_st7789v;

#endif
