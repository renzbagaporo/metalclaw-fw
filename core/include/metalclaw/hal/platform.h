// SPDX-License-Identifier: Apache-2.0 OR MIT
// The execution model, as much of it as core needs. Two locks, real time,
// and the worker: one thread of execution outside every interrupt that
// engines hand work to and that the runtime's clock ticks on. A port
// implements it with whatever it has: a second core, a task, a host thread.
//
// Lock order is runtime lock, then critical. An interrupt handler takes
// only the critical section. Both nest on one core.

#ifndef METALCLAW_HAL_PLATFORM_H
#define METALCLAW_HAL_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

// Interrupts masked on the calling core and every other core kept out.
// For image edits and engine state; never held across anything that waits.
uint32_t metalclaw_hal_critical_enter(void);
void metalclaw_hal_critical_exit(uint32_t state);

// Mutual exclusion between the script's thread and the worker for
// everything a model does outside an interrupt. Interrupts stay enabled.
// The worker only ever tries: the script's thread may wait for the worker
// while holding the lock, so the worker must never wait for the script.
void metalclaw_hal_lock(void);
bool metalclaw_hal_try_lock(void);
void metalclaw_hal_unlock(void);

// Real time since boot.
uint64_t metalclaw_hal_time_us(void);
void metalclaw_hal_delay_us(uint32_t us);

// A work item runs once on the worker per post; posting one that is
// already pending is a no-op. Safe from any context.
struct metalclaw_work {
    void (*fn)(struct metalclaw_work *work);
    void *ctx;
    volatile bool pending;
    struct metalclaw_work *volatile next;
};

void metalclaw_hal_work_post(struct metalclaw_work *work);

// Runs fn on the worker and returns once it has; inline when already there.
void metalclaw_hal_work_call(void (*fn)(void *ctx), void *ctx);

// A poll runs on every pass of the worker, at least once a millisecond.
typedef void (*metalclaw_hal_poll_t)(void *ctx);

int metalclaw_hal_poll_register(metalclaw_hal_poll_t fn, void *ctx);
bool metalclaw_hal_in_worker(void);

#endif
