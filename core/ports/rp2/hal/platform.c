// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "hardware/sync.h"
#include "pico/time.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw_port.h"

// Two hardware spinlocks, each recursive on the core that holds it. `owner`
// is only ever written by the core that took the lock, so comparing it with
// the caller's core number needs no further protection.
static spin_lock_t *critical_lock;
static spin_lock_t *runtime_lock;
static volatile int critical_owner = -1;
static volatile int runtime_owner = -1;
static uint32_t critical_depth;
static uint32_t runtime_depth;

void mclw_platform_init(void) {
    critical_lock = spin_lock_instance((uint)spin_lock_claim_unused(true));
    runtime_lock = spin_lock_instance((uint)spin_lock_claim_unused(true));
}

uint32_t mclw_hal_critical_enter(void) {
    uint32_t state = save_and_disable_interrupts();
    int core = (int)get_core_num();

    if (critical_owner == core) {
        critical_depth++;
        return state;
    }
    spin_lock_unsafe_blocking(critical_lock);
    critical_owner = core;
    critical_depth = 1;
    return state;
}

void mclw_hal_critical_exit(uint32_t state) {
    if (--critical_depth == 0) {
        critical_owner = -1;
        spin_unlock_unsafe(critical_lock);
    }
    restore_interrupts(state);
}

void mclw_hal_lock(void) {
    int core = (int)get_core_num();

    if (runtime_owner == core) {
        runtime_depth++;
        return;
    }
    spin_lock_unsafe_blocking(runtime_lock);
    runtime_owner = core;
    runtime_depth = 1;
}

bool mclw_hal_try_lock(void) {
    int core = (int)get_core_num();

    if (runtime_owner == core) {
        runtime_depth++;
        return true;
    }
    if (!spin_try_lock_unsafe(runtime_lock)) {
        return false;
    }
    runtime_owner = core;
    runtime_depth = 1;
    return true;
}

void mclw_hal_unlock(void) {
    if (--runtime_depth == 0) {
        runtime_owner = -1;
        spin_unlock_unsafe(runtime_lock);
    }
}

uint64_t mclw_hal_time_us(void) {
    return time_us_64();
}

void mclw_hal_delay_us(uint32_t us) {
    busy_wait_us_32(us);
}

void mclw_port_early_init(void) {
    mclw_platform_init();
    mclw_runtime_init();
    mclw_worker_start();
    mclw_usb_init();
}

void mclw_port_event_hook(void) {
    mclw_usb_task();
}
