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

// Drives an output pin, again after a release.
void mclw_hal_gpio_write(struct mclw_gpio *gpio, bool level);
// Stops driving an output pin, as an open-drain output does when it lets go:
// the line sits where the pin's pull and the other side leave it.
void mclw_hal_gpio_release(struct mclw_gpio *gpio);
bool mclw_hal_gpio_read(const struct mclw_gpio *gpio);
// Whether an input went low since the last call or since it was opened,
// however briefly: a pulse between two reads is not lost.
bool mclw_hal_gpio_fell(struct mclw_gpio *gpio);
uint8_t mclw_hal_gpio_pin(const struct mclw_gpio *gpio);

// An output carries a square wave from the port's own hardware: `period_ns`
// long, high for the first `high_ns` of it, until the next call; a period
// of 0 returns the pin to plain writes. -ERANGE for a period the hardware
// cannot make, -EBUSY when the hardware is held for another period. The
// wave the hardware makes is the nearest it can.
int mclw_hal_gpio_square(struct mclw_gpio *gpio, uint32_t period_ns, uint32_t high_ns);

// Links between two pins the port keeps up in hardware, with no CPU per
// edge: `to`, an output, follows every level `from` has, a buffer a few
// nanoseconds behind it; or `a` and `b` become one open-drain line, either
// side held low by what is on it is driven low on the other, and released
// sides are pulled up. A link lasts until either pin is unlinked or closed.
// -ENOSPC when the port has no engine left for it.
int mclw_hal_gpio_follow(struct mclw_gpio *to, struct mclw_gpio *from);
int mclw_hal_gpio_join(struct mclw_gpio *a, struct mclw_gpio *b);
// join for an I2C clock line: as join, and a side's release reaches the
// other side only as the whole line rises, so a target stretching the
// clock holds the controller's side low throughout. Not for SDA.
int mclw_hal_gpio_join_clock(struct mclw_gpio *a, struct mclw_gpio *b);
void mclw_hal_gpio_unlink(struct mclw_gpio *gpio);

#endif
