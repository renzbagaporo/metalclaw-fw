// SPDX-License-Identifier: Apache-2.0 OR MIT
// A PIO block reaches 32 GPIOs from its GPIO base: 0 (GPIO0-31) or, on the
// RP2350B, 16 (GPIO16-47). The base can only move while the block holds no
// program, so an engine settles it before loading one.

#ifndef MCLW_RP2_PIO_WINDOW_H
#define MCLW_RP2_PIO_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/pio.h"

#define MCLW_PIO_PIN(pin) ((uint64_t)1u << (pin))

// Every engine but the SPI target tries the blocks in one order: PIO0, then
// a third block if the chip has one, PIO1 last, since the SPI target engine
// (spi_target.c) takes PIO1 alone and needs all of its instruction memory.
// The block tried `n`th, or NULL where the chip has none.
#define MCLW_PIO_TRIES 3u

static inline PIO mclw_pio_try(size_t n) {
    static const uint8_t order[MCLW_PIO_TRIES] = { 0u, 2u, 1u };

    return n < MCLW_PIO_TRIES && order[n] < NUM_PIOS ? pio_get_instance(order[n]) : NULL;
}

// True when every pin in the mask is in the block's reach, moving the base
// of an empty block if that is what it takes.
static inline bool mclw_pio_window(PIO pio, uint64_t pins) {
#if PICO_PIO_USE_GPIO_BASE
    bool low = (pins >> 32) == 0u;
    bool high = (pins & 0xffffu) == 0u;
    uint base = pio_get_gpio_base(pio);

    if ((base == 0u && low) || (base == 16u && high)) {
        return true;
    }
    return (low || high) && pio_set_gpio_base(pio, low ? 0u : 16u) == PICO_OK;
#else
    (void)pio;
    return (pins >> 32) == 0u;
#endif
}

#endif
