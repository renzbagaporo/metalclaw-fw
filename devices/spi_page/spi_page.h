// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_SPI_PAGE_H
#define MCLW_DEVICE_SPI_PAGE_H

#include "metalclaw/model.h"

#define SPI_PAGE_SIZE 256u
// The rate the controller clocks at: what pico-examples' spi_master runs
// its PL022 at (spi_master.c spi_init(spi_default, 1000 * 1000)).
#define SPI_PAGE_HZ 1000000u
#define SPI_PAGE_INTERVAL_MS 500u
// A target count left between two pages for this long, in simulation time,
// is a page cut short (a transfer shorter than a page, or the DUT reset
// under one): the rings start again at position 0, keeping what they hold.
#define SPI_PAGE_REALIGN_US 5000u

extern const struct mclw_model mclw_model_spi_page_target;
extern const struct mclw_model mclw_model_spi_page_controller;

#endif
