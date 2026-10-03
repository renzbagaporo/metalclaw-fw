// SPDX-License-Identifier: Apache-2.0 OR MIT
// A single pin driven or sampled by a device. The port owns the object; the
// model only holds the handle it was given at init.

#ifndef MCLW_HAL_GPIO_H
#define MCLW_HAL_GPIO_H

#include <stdbool.h>
#include <stdint.h>

struct mclw_gpio;

enum mclw_gpio_pull {
    MCLW_GPIO_PULL_NONE,
    MCLW_GPIO_PULL_UP,
    MCLW_GPIO_PULL_DOWN,
};

// Claims the pin for `owner`. Output pins start at `initial`. Returns NULL
// with *err set to a negative errno when the pin cannot be had.
struct mclw_gpio *mclw_hal_gpio_open(uint8_t pin, bool output, enum mclw_gpio_pull pull,
    bool initial, const void *owner, int *err);
void mclw_hal_gpio_close(struct mclw_gpio *gpio);

void mclw_hal_gpio_write(struct mclw_gpio *gpio, bool level);
bool mclw_hal_gpio_read(const struct mclw_gpio *gpio);
// Whether an input went low since the last call or since it was opened,
// however briefly: a pulse between two reads is not lost.
bool mclw_hal_gpio_fell(struct mclw_gpio *gpio);
uint8_t mclw_hal_gpio_pin(const struct mclw_gpio *gpio);

#endif
