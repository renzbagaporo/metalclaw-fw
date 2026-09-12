// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>

#include "hardware/gpio.h"
#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/pins.h"

#ifndef METALCLAW_GPIO_MAX
#define METALCLAW_GPIO_MAX 16
#endif

struct metalclaw_gpio {
    uint8_t pin;
    bool output;
    const void *owner;
};

static struct metalclaw_gpio slots[METALCLAW_GPIO_MAX];

struct metalclaw_gpio *metalclaw_hal_gpio_open(uint8_t pin, bool output, enum metalclaw_gpio_pull pull,
    bool initial, const void *owner, int *err) {
    struct metalclaw_gpio *g = NULL;
    for (size_t i = 0; i < METALCLAW_GPIO_MAX; i++) {
        if (slots[i].owner == NULL) {
            g = &slots[i];
            break;
        }
    }
    if (g == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    *err = metalclaw_hal_pin_claim(pin, owner);
    if (*err != 0) {
        return NULL;
    }
    g->pin = pin;
    g->output = output;
    g->owner = owner;

    gpio_init(pin);
    gpio_set_pulls(pin, pull == METALCLAW_GPIO_PULL_UP, pull == METALCLAW_GPIO_PULL_DOWN);
    if (output) {
        gpio_put(pin, initial);
        gpio_set_dir(pin, GPIO_OUT);
    } else {
        gpio_set_dir(pin, GPIO_IN);
    }
    return g;
}

void metalclaw_hal_gpio_close(struct metalclaw_gpio *g) {
    if (g == NULL || g->owner == NULL) {
        return;
    }
    gpio_deinit(g->pin);
    metalclaw_hal_pin_release(g->pin, g->owner);
    g->owner = NULL;
}

void metalclaw_hal_gpio_write(struct metalclaw_gpio *g, bool level) {
    if (g->output) {
        gpio_put(g->pin, level);
    }
}

bool metalclaw_hal_gpio_read(const struct metalclaw_gpio *g) {
    return gpio_get(g->pin);
}

uint8_t metalclaw_hal_gpio_pin(const struct metalclaw_gpio *g) {
    return g->pin;
}
