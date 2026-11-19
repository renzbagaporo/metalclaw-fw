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
#define WORKER_POLLS_MAX MCLW_WORKER_POLLS_MAX

struct poll_entry {
    mclw_hal_poll_t fn;
    void *ctx;
};

static uint32_t worker_stack[WORKER_STACK_WORDS] __attribute__((aligned(8)));
static struct mclw_work *volatile work_head;
static struct poll_entry polls[WORKER_POLLS_MAX];
static volatile size_t poll_count;
static uint tick_alarm;
static struct {
    uint32_t rounds;
    uint32_t last_round_us;
    uint32_t max_round_gap_us;
    uint32_t max_work_per_round;
    uint32_t poll_max_us[WORKER_POLLS_MAX];
} stats;
volatile uint32_t mclw_worker_lock_misses;

struct call_context {
    void (*fn)(void *ctx);
    void *ctx;
    volatile bool done;
};

static void __not_in_flash_func(tick_handler)(uint alarm) {
    hardware_alarm_set_target(alarm, make_timeout_time_us(WORKER_TICK_US));
}

// One round: the work queued as it starts. Work posted meanwhile, a
// callback's repost of itself included, waits for the next round, after the
// polls. Draining until empty starved the polls, and the clock with them,
// whenever work came in faster than it was done: pico-examples' spi_flash
// polling a w25q's status back to back, every window posting its delivery,
// kept an RP2040 worker draining for good, and the erase never ended.
static void drain(void) {
    uint32_t state = mclw_hal_critical_enter();
    struct mclw_work *w = work_head;
    uint32_t now = time_us_32();
    uint32_t ran = 0;

    work_head = NULL;
    mclw_hal_critical_exit(state);
    if (stats.rounds != 0u && now - stats.last_round_us > stats.max_round_gap_us) {
        stats.max_round_gap_us = now - stats.last_round_us;
    }
    stats.last_round_us = now;
    stats.rounds++;
    while (w != NULL) {
        ran++;
        struct mclw_work *next;

        state = mclw_hal_critical_enter();
        next = w->next;
        w->next = NULL;
        w->pending = false;
        mclw_hal_critical_exit(state);
        w->fn(w);
        w = next;
    }
    if (ran > stats.max_work_per_round) {
        stats.max_work_per_round = ran;
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
            uint32_t start = time_us_32();
            uint32_t took;

            polls[i].fn(polls[i].ctx);
            took = time_us_32() - start;
            if (took > stats.poll_max_us[i]) {
                stats.poll_max_us[i] = took;
            }
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

// Read from the script's thread while the worker writes: each field is one
// word, so a read is a value the worker held, if not all from one moment.
void mclw_hal_worker_stats(struct mclw_worker_stats *out, bool reset) {
    size_t n = poll_count;

    out->rounds = stats.rounds;
    out->max_round_gap_us = stats.max_round_gap_us;
    out->max_work_per_round = stats.max_work_per_round;
    out->lock_misses = mclw_worker_lock_misses;
    out->poll_count = (uint32_t)n;
    for (size_t i = 0; i < WORKER_POLLS_MAX; i++) {
        out->poll_fn[i] = i < n ? (uintptr_t)polls[i].fn : 0u;
        out->poll_max_us[i] = i < n ? stats.poll_max_us[i] : 0u;
    }
    if (reset) {
        stats.max_round_gap_us = 0;
        stats.max_work_per_round = 0;
        mclw_worker_lock_misses = 0;
        for (size_t i = 0; i < WORKER_POLLS_MAX; i++) {
            stats.poll_max_us[i] = 0;
        }
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

struct poll_removal {
    mclw_hal_poll_t fn;
    void *ctx;
};

static void poll_remove(void *arg) {
    const struct poll_removal *r = arg;
    size_t n = poll_count;

    for (size_t i = 0; i < n; i++) {
        if (polls[i].fn == r->fn && polls[i].ctx == r->ctx) {
            for (size_t j = i; j + 1u < n; j++) {
                polls[j] = polls[j + 1u];
                stats.poll_max_us[j] = stats.poll_max_us[j + 1u];
            }
            stats.poll_max_us[n - 1u] = 0;
            __dmb();
            poll_count = n - 1u;
            return;
        }
    }
}

void mclw_hal_poll_unregister(mclw_hal_poll_t fn, void *ctx) {
    struct poll_removal r = { .fn = fn, .ctx = ctx };

    mclw_hal_work_call(poll_remove, &r);
}
