// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_HUB75_H
#define MCLW_DEVICE_HUB75_H

#include "metalclaw/model.h"

// A 128 x 64 panel scanned 1/32 (pico-examples pio/hub75/Readme.md): five
// address lines select a row of the upper half (R0, G0, B0) and the same
// row of the lower half (R1, G1, B1).
#define HUB75_COLUMNS 128u
#define HUB75_ROWS 64u
#define HUB75_SCAN_ROWS 32u
// Distinct enabled times kept per row: one per bit plane of an 8-bit
// binary-weighted drive.
#define HUB75_SLOTS 8u
// The model keeps every eighth column.
#define HUB75_THUMB_STEP 8u
#define HUB75_THUMB_COLUMNS (HUB75_COLUMNS / HUB75_THUMB_STEP)
#define HUB75_SLOT_BYTES (4u + HUB75_THUMB_COLUMNS)
#define HUB75_ROW_BYTES (HUB75_SLOTS * HUB75_SLOT_BYTES)
#define HUB75_PLANES_BYTES (HUB75_SCAN_ROWS * HUB75_ROW_BYTES)
#define HUB75_THUMBNAIL_BYTES (HUB75_ROWS * HUB75_THUMB_COLUMNS * 3u)
// What one capture can bring: 512 samples and the enabled time.
#define HUB75_CAPTURE_WORDS 513u

extern const struct mclw_model mclw_model_hub75;

#endif
