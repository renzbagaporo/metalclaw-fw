// SPDX-License-Identifier: Apache-2.0 OR MIT
// A single pin driven or sampled by a device. The port owns the object; the
// model only holds the handle it was given at init.

#ifndef METALCLAW_HAL_GPIO_H
#define METALCLAW_HAL_GPIO_H

#include <stdbool.h>
#include <stdint.h>

struct metalclaw_gpio;

enum metalclaw_gpio_pull {
    METALCLAW_GPIO_PULL_NONE,
    METALCLAW_GPIO_PULL_UP,
    METALCLAW_GPIO_PULL_DOWN,
};

// Claims the pin for `owner`. Output pins start at `initial`. Returns NULL
// with *err set to a negative errno when the pin cannot be had.
struct metalclaw_gpio *metalclaw_hal_gpio_open(uint8_t pin, bool output, enum metalclaw_gpio_pull pull,
    bool initial, const void *owner, int *err);
void metalclaw_hal_gpio_close(struct metalclaw_gpio *gpio);

void metalclaw_hal_gpio_write(struct metalclaw_gpio *gpio, bool level);
bool metalclaw_hal_gpio_read(const struct metalclaw_gpio *gpio);
uint8_t metalclaw_hal_gpio_pin(const struct metalclaw_gpio *gpio);

#endif
