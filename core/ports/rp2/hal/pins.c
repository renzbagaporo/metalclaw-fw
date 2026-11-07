// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>

#include "hardware/platform_defs.h"
#include "py/mpconfig.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"

#define PIN_COUNT NUM_BANK0_GPIOS

static const uint8_t reserved[] = { METALCLAW_RESERVED_PINS MCLW_PIN_NONE };
static const void *owners[PIN_COUNT];

uint8_t mclw_hal_pin_count(void) {
    return PIN_COUNT;
}

bool mclw_hal_pin_reserved(uint8_t pin) {
    for (size_t i = 0; reserved[i] != MCLW_PIN_NONE; i++) {
        if (reserved[i] == pin) {
            return true;
        }
    }
    return false;
}

int mclw_hal_pin_claim(uint8_t pin, const void *owner) {
    if (pin >= PIN_COUNT || owner == NULL || mclw_hal_pin_reserved(pin)) {
        return -EINVAL;
    }
    int err = 0;
    uint32_t state = mclw_hal_critical_enter();
    if (owners[pin] == NULL || owners[pin] == owner) {
        owners[pin] = owner;
    } else {
        err = -EBUSY;
    }
    mclw_hal_critical_exit(state);
    return err;
}

void mclw_hal_pin_release(uint8_t pin, const void *owner) {
    if (pin >= PIN_COUNT) {
        return;
    }
    uint32_t state = mclw_hal_critical_enter();
    if (owners[pin] == owner) {
        owners[pin] = NULL;
    }
    mclw_hal_critical_exit(state);
}

void mclw_hal_pin_release_all(const void *owner) {
    uint32_t state = mclw_hal_critical_enter();
    for (size_t i = 0; i < PIN_COUNT; i++) {
        if (owners[i] == owner) {
            owners[i] = NULL;
        }
    }
    mclw_hal_critical_exit(state);
}
