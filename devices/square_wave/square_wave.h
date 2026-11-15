// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_SQUARE_WAVE_H
#define MCLW_DEVICE_SQUARE_WAVE_H

#include "metalclaw/model.h"

// The shortest period the model makes on the simulation clock, whose step
// is a millisecond: each half then lands within one step of its time.
// Shorter periods come from the port's hardware.
#define SQUARE_WAVE_CLOCK_MIN_NS 2000000u

extern const struct mclw_model mclw_model_square_wave;

#endif
