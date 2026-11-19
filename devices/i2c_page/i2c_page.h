// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_I2C_PAGE_H
#define MCLW_DEVICE_I2C_PAGE_H

#include "metalclaw/model.h"

#define I2C_PAGE_SIZE 256u
#define I2C_PAGE_INTERVAL_MS 500u
#define I2C_PAGE_RATE_KHZ 100u
#define I2C_PAGE_CHUNK 16u

extern const struct mclw_model mclw_model_i2c_page_controller;

#endif
