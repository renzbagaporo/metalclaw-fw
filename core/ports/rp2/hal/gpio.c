// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>

#include "hardware/gpio.h"
#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/pins.h"

#ifndef MCLW_GPIO_MAX
#define MCLW_GPIO_MAX 16
#endif

struct mclw_gpio {
    uint8_t pin;
    bool output;
    const void *owner;
};

static struct mclw_gpio slots[MCLW_GPIO_MAX];

struct mclw_gpio *mclw_hal_gpio_open(uint8_t pin, bool output, enum mclw_gpio_pull pull,
    bool initial, const void *owner, int *err) {
    struct mclw_gpio *g = NULL;
    for (size_t i = 0; i < MCLW_GPIO_MAX; i++) {
        if (slots[i].owner == NULL) {
            g = &slots[i];
            break;
        }
    }
    if (g == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    *err = mclw_hal_pin_claim(pin, owner);
    if (*err != 0) {
        return NULL;
    }
    g->pin = pin;
    g->output = output;
    g->owner = owner;

    gpio_init(pin);
    gpio_set_pulls(pin, pull == MCLW_GPIO_PULL_UP, pull == MCLW_GPIO_PULL_DOWN);
    if (output) {
        gpio_put(pin, initial);
        gpio_set_dir(pin, GPIO_OUT);
    } else {
        gpio_set_dir(pin, GPIO_IN);
        gpio_acknowledge_irq(pin, GPIO_IRQ_EDGE_FALL);
    }
    return g;
}

void mclw_hal_gpio_close(struct mclw_gpio *g) {
    if (g == NULL || g->owner == NULL) {
        return;
    }
    gpio_deinit(g->pin);
    mclw_hal_pin_release(g->pin, g->owner);
    g->owner = NULL;
}

void mclw_hal_gpio_write(struct mclw_gpio *g, bool level) {
    if (g->output) {
        gpio_put(g->pin, level);
    }
}

bool mclw_hal_gpio_read(const struct mclw_gpio *g) {
    return gpio_get(g->pin);
}

// The bank's raw edge latch holds a fall whether or not any interrupt is
// enabled for the pin. MicroPython's GPIO interrupt handler acknowledges
// every pin's latches when a Pin.irq fires, so with one in use a fall can
// still go unseen.
bool mclw_hal_gpio_fell(struct mclw_gpio *g) {
    uint32_t events = io_bank0_hw->intr[g->pin / 8u] >> (4u * (g->pin % 8u));

    if ((events & GPIO_IRQ_EDGE_FALL) == 0u) {
        return false;
    }
    gpio_acknowledge_irq(g->pin, GPIO_IRQ_EDGE_FALL);
    return true;
}

uint8_t mclw_hal_gpio_pin(const struct mclw_gpio *g) {
    return g->pin;
}
