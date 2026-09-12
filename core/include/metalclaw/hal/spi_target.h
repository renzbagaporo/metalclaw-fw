// SPDX-License-Identifier: Apache-2.0 OR MIT
// An SPI target (slave) that serves an image from hardware. The model arms
// either a register map, framed the way the part's datasheet says, or a
// stream of fixed frames; the engine answers every chip-select window
// without the model. The DUT is the master and sets the clock; the engine
// publishes the ceiling it was qualified at for the armed image.

#ifndef METALCLAW_HAL_SPI_TARGET_H
#define METALCLAW_HAL_SPI_TARGET_H

#include <stdbool.h>
#include <stdint.h>

#include "metalclaw/regmap.h"
#include "metalclaw/stream.h"

struct metalclaw_spi_target;

// How the part frames a register access, from its datasheet: which
// command-byte bit means read, which bits are the address, whether a
// multi-byte access walks consecutive addresses, and how many don't-care
// bytes the part clocks out before read data.
struct metalclaw_regmap_framing {
    uint8_t read_bit_mask;
    uint8_t address_mask;
    uint8_t read_dummy_bytes;
    bool auto_increment;
};

struct metalclaw_spi_target_stats {
    uint32_t transactions;
    uint32_t bytes;
    // Windows that closed before a whole command or frame was clocked.
    uint32_t short_windows;
    // Windows that clocked more than the image could answer.
    uint32_t long_windows;
    uint32_t dma_faults;
    // Windows served before a pending image rebuild had landed.
    uint32_t stale_windows;
};

// Claims the four pins for `owner`. `mode` is SPI mode 0..3. The target
// answers filler until armed.
struct metalclaw_spi_target *metalclaw_hal_spi_target_open(uint8_t sck, uint8_t mosi, uint8_t miso, uint8_t cs,
    uint8_t mode, bool msb_first, const void *owner, int *err);
void metalclaw_hal_spi_target_close(struct metalclaw_spi_target *t);
int metalclaw_hal_spi_target_set_mode(struct metalclaw_spi_target *t, uint8_t mode);

// The image must outlive the arm. Arming replaces whatever was armed. A
// read dummy byte (at most one) is clocked out by the state machine itself
// so the first data byte has a byte time to arrive, which is why the map's
// filler must then be 0x00 or 0xff.
int metalclaw_hal_spi_target_arm_regmap(struct metalclaw_spi_target *t, struct metalclaw_regmap *map,
    const struct metalclaw_regmap_framing *framing);
int metalclaw_hal_spi_target_arm_stream(struct metalclaw_spi_target *t, struct metalclaw_stream *stream);
void metalclaw_hal_spi_target_disarm(struct metalclaw_spi_target *t);

// Called from the port's idle hook; a lost chip-select edge is recovered here.
void metalclaw_hal_spi_target_poll(void);

void metalclaw_hal_spi_target_stats(const struct metalclaw_spi_target *t, struct metalclaw_spi_target_stats *out);
uint32_t metalclaw_hal_spi_target_max_clock_hz(const struct metalclaw_spi_target *t);

#endif
