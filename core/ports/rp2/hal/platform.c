// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "hardware/sync.h"
#include "pico/time.h"
#if !PICO_RP2040
#include "hardware/structs/qmi.h"
#include "hardware/watchdog.h"
#endif
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

// Both halves live in RAM: the SPI engine's interrupts enter the critical
// section, and a fetch from flash that misses the cache, with the other core
// running MicroPython, would delay the engine's re-arm by microseconds.
uint32_t __not_in_flash_func(mclw_hal_critical_enter)(void) {
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

void __not_in_flash_func(mclw_hal_critical_exit)(uint32_t state) {
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
        extern volatile uint32_t mclw_worker_lock_misses;
        if (core == 1) {
            mclw_worker_lock_misses = mclw_worker_lock_misses + 1u;
        }
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

#if !PICO_RP2040
// A watchdog scratch word marking the reboot below, so it happens once.
#define XIP_REBOOT_SCRATCH 3u
#define XIP_REBOOT_MARK 0x58495052u

// A debugger's flash loader can leave the RP2350's QMI windows in slow
// reads: M0 (the flash) in plain serial reads (03h), or M1 (the CS1 window,
// a PSRAM's) in a dual read (BBh) where the boot path sets the quad EBh it
// sets on M0. A reset-and-go then starts the image without the boot path
// that sets them up, and everything run or kept there is many times slower
// (the worker took 200 us a clock poll, against 20). Rebooting once through
// the bootrom, as a watchdog reset does, sets them up.
static uint32_t rcmd_prefix(uint32_t rcmd) {
    return (rcmd & QMI_M0_RCMD_PREFIX_BITS) >> QMI_M0_RCMD_PREFIX_LSB;
}

static void xip_fast_or_reboot(void) {
    uint32_t m0 = rcmd_prefix(qmi_hw->m[0].rcmd);
    uint32_t m1 = rcmd_prefix(qmi_hw->m[1].rcmd);
    bool slow = m0 == 0x03u || (m1 != 0u && m1 != m0);

    if (!slow || watchdog_hw->scratch[XIP_REBOOT_SCRATCH] == XIP_REBOOT_MARK) {
        watchdog_hw->scratch[XIP_REBOOT_SCRATCH] = 0u;
        return;
    }
    watchdog_hw->scratch[XIP_REBOOT_SCRATCH] = XIP_REBOOT_MARK;
    watchdog_reboot(0u, 0u, 0u);
    for (;;) {
        tight_loop_contents();
    }
}
#endif

void mclw_port_early_init(void) {
#if !PICO_RP2040
    xip_fast_or_reboot();
#endif
    mclw_platform_init();
    mclw_runtime_init();
    mclw_worker_start();
    mclw_usb_init();
}

void mclw_port_event_hook(void) {
    mclw_usb_task();
}
