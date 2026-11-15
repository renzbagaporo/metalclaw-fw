// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_LCD_BACKPACK_H
#define MCLW_DEVICE_LCD_BACKPACK_H

#include "metalclaw/model.h"

// The largest panel the firmware drives (its displaybuff[20][4]); the
// display and splash regions are this many rows of this many bytes.
#define LCD_BACKPACK_COLS_MAX 20
#define LCD_BACKPACK_ROWS_MAX 4

extern const struct mclw_model mclw_model_lcd_backpack;

#endif
