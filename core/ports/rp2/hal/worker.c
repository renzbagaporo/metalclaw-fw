// SPDX-License-Identifier: Apache-2.0 OR MIT
// The worker is core 1. It drains posted work, runs every registered poll,
// then sleeps on WFE; a post from core 0 wakes it with SEV, an interrupt on
// this core wakes it by itself, and a hardware alarm wakes it every
// millisecond so the polls run on time. It calls
// multicore_lockout_victim_init so MicroPython's flash writes park it, and
// it is never reset: a soft reset leaves it running.

#include <stddef.h>

#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico/multicore.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw_port.h"

#define WORKER_STACK_WORDS 2048u
#define WORKER_TICK_US 1000u
#define WORKER_POLLS_MAX 8u

struct poll_entry {
    mclw_hal_poll_t fn;
    void *ctx;
};

static uint32_t worker_stack[WORKER_STACK_WORDS] __attribute__((aligned(8)));
static struct mclw_work *volatile work_head;
static struct poll_entry polls[WORKER_POLLS_MAX];
static volatile size_t poll_count;
static uint tick_alarm;

struct call_context {
    void (*fn)(void *ctx);
    void *ctx;
    volatile bool done;
};

static void __not_in_flash_func(tick_handler)(uint alarm) {
    hardware_alarm_set_target(alarm, make_timeout_time_us(WORKER_TICK_US));
}

static void drain(void) {
    for (;;) {
        uint32_t state = mclw_hal_critical_enter();
        struct mclw_work *w = work_head;

        if (w != NULL) {
            work_head = w->next;
            w->next = NULL;
            w->pending = false;
        }
        mclw_hal_critical_exit(state);
        if (w == NULL) {
            return;
        }
        w->fn(w);
    }
}

static void worker_main(void) {
    multicore_lockout_victim_init();
    tick_alarm = (uint)hardware_alarm_claim_unused(true);
    hardware_alarm_set_callback(tick_alarm, tick_handler);
    hardware_alarm_set_target(tick_alarm, make_timeout_time_us(WORKER_TICK_US));
    for (;;) {
        drain();
        for (size_t i = 0; i < poll_count; i++) {
            polls[i].fn(polls[i].ctx);
        }
        if (work_head == NULL) {
            __wfe();
        }
    }
}

void mclw_worker_start(void) {
    // A debugger reset leaves core 1 wherever it was; the launch handshake
    // only completes from the bootrom's wait loop, which the PSM reset
    // puts it back into.
    multicore_reset_core1();
    multicore_launch_core1_with_stack(worker_main, worker_stack, sizeof(worker_stack));
}

bool mclw_hal_in_worker(void) {
    return get_core_num() == 1;
}

void mclw_hal_work_post(struct mclw_work *work) {
    uint32_t state = mclw_hal_critical_enter();

    if (!work->pending) {
        work->pending = true;
        work->next = NULL;
        if (work_head == NULL) {
            work_head = work;
        } else {
            struct mclw_work *tail = work_head;
            while (tail->next != NULL) {
                tail = tail->next;
            }
            tail->next = work;
        }
    }
    mclw_hal_critical_exit(state);
    __sev();
}

static void call_trampoline(struct mclw_work *work) {
    struct call_context *c = work->ctx;

    c->fn(c->ctx);
    __dmb();
    c->done = true;
    __sev();
}

void mclw_hal_work_call(void (*fn)(void *ctx), void *ctx) {
    if (mclw_hal_in_worker()) {
        fn(ctx);
        return;
    }
    struct call_context c = { .fn = fn, .ctx = ctx, .done = false };
    struct mclw_work w = { .fn = call_trampoline, .ctx = &c, .pending = false, .next = NULL };

    mclw_hal_work_post(&w);
    while (!c.done) {
        __wfe();
    }
}

int mclw_hal_poll_register(mclw_hal_poll_t fn, void *ctx) {
    uint32_t state = mclw_hal_critical_enter();

    if (poll_count >= WORKER_POLLS_MAX) {
        mclw_hal_critical_exit(state);
        return -1;
    }
    polls[poll_count].fn = fn;
    polls[poll_count].ctx = ctx;
    __dmb();
    poll_count++;
    mclw_hal_critical_exit(state);
    return 0;
}
