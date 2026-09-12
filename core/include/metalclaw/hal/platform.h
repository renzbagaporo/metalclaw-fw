// SPDX-License-Identifier: Apache-2.0 OR MIT
// The execution model, as much of it as core needs. A port implements these
// with whatever it has: interrupt masking on a bare core, a scheduler lock
// under an RTOS.

#ifndef METALCLAW_HAL_PLATFORM_H
#define METALCLAW_HAL_PLATFORM_H

#include <stdint.h>

// Short, non-nesting-aware critical section. Safe from any context.
uint32_t metalclaw_hal_critical_enter(void);
void metalclaw_hal_critical_exit(uint32_t state);

#endif
