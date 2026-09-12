// SPDX-License-Identifier: Apache-2.0 OR MIT
// Pin ownership. Every HAL engine claims its pins here before touching them,
// and the board says which pins are never available.

#ifndef METALCLAW_HAL_PINS_H
#define METALCLAW_HAL_PINS_H

#include <stdbool.h>
#include <stdint.h>

#define METALCLAW_PIN_NONE 0xffu

// -EINVAL if the pin does not exist or is reserved, -EBUSY if owned by
// someone else, 0 on success. Claiming a pin one already owns succeeds.
int metalclaw_hal_pin_claim(uint8_t pin, const void *owner);
void metalclaw_hal_pin_release(uint8_t pin, const void *owner);
void metalclaw_hal_pin_release_all(const void *owner);
bool metalclaw_hal_pin_reserved(uint8_t pin);
uint8_t metalclaw_hal_pin_count(void);

#endif
