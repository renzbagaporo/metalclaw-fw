// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_QUADRATURE_ENCODER_H
#define MCLW_DEVICE_QUADRATURE_ENCODER_H

#include <stdint.h>

#include "metalclaw/model.h"

// 128 steps a millisecond fill the engine's FIFO in one poll.
#define QUADRATURE_ENCODER_RATE_MAX 100000

uint32_t quadrature_encoder_phase(int32_t position);

extern const struct mclw_model mclw_model_quadrature_encoder;

#endif
