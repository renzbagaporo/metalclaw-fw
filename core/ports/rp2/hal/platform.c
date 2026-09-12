// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "hardware/sync.h"
#include "metalclaw/hal/platform.h"

uint32_t metalclaw_hal_critical_enter(void) {
    return save_and_disable_interrupts();
}

void metalclaw_hal_critical_exit(uint32_t state) {
    restore_interrupts(state);
}
