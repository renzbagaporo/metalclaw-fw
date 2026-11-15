// SPDX-License-Identifier: Apache-2.0 OR MIT
// A receive-only SPI target for a part that never drives MISO: a display
// controller, an LED driver. It captures every byte the host clocks while
// chip select is low, with the level of the part's side line (a
// data/command select) as the byte's last bit is clocked, and delivers
// them to a sink (metalclaw/sink.h). Chip select may rise between every
// byte, as a PL022 does in Motorola mode with CPHA 0, or stay low for a
// whole frame; nothing is re-armed between windows, so neither costs a
// byte.

#ifndef MCLW_HAL_SPI_SINK_H
#define MCLW_HAL_SPI_SINK_H

#include <stdbool.h>
#include <stdint.h>

#include "metalclaw/sink.h"

struct mclw_spi_sink;

struct mclw_spi_sink_stats {
    uint32_t bytes;
    uint32_t windows;
    // Bytes the engine captured that were overwritten before the worker
    // delivered them.
    uint32_t overruns;
};

// Claims the pins for `owner`; `line` may be MCLW_PIN_NONE. `mode` is SPI
// mode 0..3. Nothing is captured until armed.
struct mclw_spi_sink *mclw_hal_spi_sink_open(uint8_t sck, uint8_t mosi, uint8_t cs, uint8_t line,
    uint8_t mode, bool msb_first, const void *owner, int *err);
void mclw_hal_spi_sink_close(struct mclw_spi_sink *s);

// The sink must outlive the arm. Capture starts at the next chip-select
// fall; arming replaces whatever was armed.
int mclw_hal_spi_sink_arm(struct mclw_spi_sink *s, struct mclw_sink *sink);
void mclw_hal_spi_sink_disarm(struct mclw_spi_sink *s);

void mclw_hal_spi_sink_stats(const struct mclw_spi_sink *s, struct mclw_spi_sink_stats *out);
uint32_t mclw_hal_spi_sink_max_clock_hz(const struct mclw_spi_sink *s);

#endif
