// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_HT16K33_H
#define MCLW_DEVICE_HT16K33_H

#include "metalclaw/model.h"

// The display RAM: 16 x 8 bits, address 2n for ROW0-7 and 2n + 1 for
// ROW8-15 of COMn (p. 13).
#define HT16K33_DISPLAY_BYTES 16u

extern const struct mclw_model mclw_model_ht16k33;

#endif
