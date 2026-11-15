// SPDX-License-Identifier: Apache-2.0 OR MIT
// The pin object behind hal/gpio.h, shared by plain pins and the PWM square
// wave (gpio.c), the PIO links between pins (gpio_link.c) and the SPI peers
// built on pins (spi_peer.c). A pin serves one of them at a time.

#ifndef MCLW_RP2_GPIO_INTERNAL_H
#define MCLW_RP2_GPIO_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "metalclaw/hal/gpio.h"

struct mclw_gpio {
    uint8_t pin;
    bool output;
    bool square;
    enum mclw_gpio_pull pull;
    const void *owner;
    // The link this pin takes part in, or NULL.
    struct gpio_link *link;
    // An SPI peer drives the pin; the model closes it before the pin.
    bool held;
};

// Back to what the pin was opened as: SIO, its direction, its pull.
void mclw_rp2_gpio_restore(struct mclw_gpio *gpio);

#endif
