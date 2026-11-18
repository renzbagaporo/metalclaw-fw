// SPDX-License-Identifier: Apache-2.0 OR MIT
// Pin ownership for the host port. The pins are the DUT's: the bridge
// names every line by the DUT pin it is wired to, as a harness on a bench
// is wired one to one. Enough for an RP2350B's 48.

#include <errno.h>
#include <stddef.h>

#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"

#define PIN_COUNT 48u

static const void *owners[PIN_COUNT];

uint8_t mclw_hal_pin_count(void) {
    return PIN_COUNT;
}

bool mclw_hal_pin_reserved(uint8_t pin) {
    (void)pin;
    return false;
}

int mclw_hal_pin_claim(uint8_t pin, const void *owner) {
    int err = 0;

    if (pin >= PIN_COUNT || owner == NULL) {
        return -EINVAL;
    }
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
