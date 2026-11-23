// SPDX-License-Identifier: Apache-2.0 OR MIT
// Locks, time and the worker for the host port. The critical section and
// the runtime lock are recursive mutexes; the worker is one thread, which
// in bridged mode is also the bridge's. See host.h.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "metalclaw/hal/platform.h"
#include "metalclaw/runtime.h"

#include "host.h"

#define WORKER_TICK_US 1000u
// Bridged, how long a round that missed the runtime lock waits for it
// between tries, and how many tries it makes before the DUT's time goes on
// regardless (about a second).
#define SETTLE_WAIT_NS 50000L
#define SETTLE_TRIES 20000u

struct poll_entry {
    mclw_hal_poll_t fn;
    void *ctx;
};

static pthread_mutex_t critical_mutex;
static pthread_mutex_t runtime_mutex;
static pthread_t worker_thread;
static int wake_pipe[2] = { -1, -1 };

static struct mclw_work *work_head;
static struct poll_entry polls[MCLW_WORKER_POLLS_MAX];
static size_t poll_count;

// Threads other than the worker waiting for the runtime lock. A mutex is
// not fair: the worker, which takes and drops the lock once a request,
// could win it every time. While anyone waits, its tries fail.
static atomic_int lock_waiters;

static bool bridged;
static uint64_t start_ns;
// Bridged: the DUT's virtual time, and what delay_us added to it for the
// events the code running now makes; each round, the posted work and each
// request start it from zero.
static _Atomic uint64_t virtual_ns;
static uint64_t delay_ns;

// The worker's failed tries of the runtime lock, never reset: a bridged
// round runs again until it makes none.
static uint32_t worker_misses;

static struct {
    uint32_t rounds;
    uint64_t last_round_us;
    uint32_t max_round_gap_us;
    uint32_t max_work_per_round;
    uint32_t lock_misses;
    uint32_t poll_max_us[MCLW_WORKER_POLLS_MAX];
} stats;

static pthread_mutex_t call_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t call_done = PTHREAD_COND_INITIALIZER;

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

bool mclw_host_bridged(void) {
    return bridged;
}

uint32_t mclw_hal_critical_enter(void) {
    pthread_mutex_lock(&critical_mutex);
    return 0;
}

void mclw_hal_critical_exit(uint32_t state) {
    (void)state;
    pthread_mutex_unlock(&critical_mutex);
}

void mclw_hal_lock(void) {
    if (mclw_hal_in_worker()) {
        pthread_mutex_lock(&runtime_mutex);
        return;
    }
    atomic_fetch_add(&lock_waiters, 1);
    pthread_mutex_lock(&runtime_mutex);
    atomic_fetch_sub(&lock_waiters, 1);
}

bool mclw_hal_try_lock(void) {
    if (mclw_hal_in_worker() && atomic_load(&lock_waiters) != 0) {
        stats.lock_misses++;
        worker_misses++;
        return false;
    }
    if (pthread_mutex_trylock(&runtime_mutex) != 0) {
        if (mclw_hal_in_worker()) {
            stats.lock_misses++;
            worker_misses++;
        }
        return false;
    }
    return true;
}

void mclw_hal_unlock(void) {
    pthread_mutex_unlock(&runtime_mutex);
}

uint64_t mclw_hal_time_us(void) {
    if (bridged) {
        return atomic_load(&virtual_ns) / 1000u;
    }
    return (monotonic_ns() - start_ns) / 1000u;
}

// Bridged, on the worker: no waiting, since the DUT's time stands still
// while a request is handled; the events made after it happen that much
// later. A pulse a model makes with write, delay, write reaches the DUT
// with its width.
void mclw_hal_delay_us(uint32_t us) {
    if (bridged && mclw_hal_in_worker()) {
        delay_ns += (uint64_t)us * 1000u;
        return;
    }
    struct timespec ts = { .tv_sec = us / 1000000u, .tv_nsec = (long)(us % 1000000u) * 1000 };
    nanosleep(&ts, NULL);
}

uint64_t mclw_host_event_ns(void) {
    return atomic_load(&virtual_ns) + delay_ns;
}

bool mclw_hal_in_worker(void) {
    return pthread_equal(pthread_self(), worker_thread) != 0;
}

static void wake(void) {
    uint8_t one = 1;

    if (wake_pipe[1] >= 0) {
        (void)!write(wake_pipe[1], &one, 1);
    }
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
    wake();
}

void mclw_host_drain_work(void) {
    uint32_t state = mclw_hal_critical_enter();
    struct mclw_work *w = work_head;
    uint32_t ran = 0;

    work_head = NULL;
    mclw_hal_critical_exit(state);
    while (w != NULL) {
        struct mclw_work *next;

        ran++;
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

static void round_run(void) {
    uint64_t now = mclw_hal_time_us();
    size_t n;

    if (stats.rounds != 0u && now - stats.last_round_us > stats.max_round_gap_us) {
        stats.max_round_gap_us = (uint32_t)(now - stats.last_round_us);
    }
    stats.last_round_us = now;
    stats.rounds++;
    mclw_host_drain_work();
    n = poll_count;
    for (size_t i = 0; i < n; i++) {
        uint64_t began = monotonic_ns();
        uint32_t took;

        polls[i].fn(polls[i].ctx);
        took = (uint32_t)((monotonic_ns() - began) / 1000u);
        if (took > stats.poll_max_us[i]) {
            stats.poll_max_us[i] = took;
        }
    }
}

// A round whose polls missed the runtime lock, because a script thread
// held it or waited for it, runs again before the DUT's time goes on: the
// runtime's tick would otherwise reach the models late in the DUT's time,
// later the busier the host. The posted work runs between tries, so a
// script waiting on the worker while it holds the lock gets its answer.
static void round_settled(void) {
    uint32_t misses = worker_misses;

    round_run();
    for (uint32_t tries = 0; worker_misses != misses && tries < SETTLE_TRIES; tries++) {
        struct timespec wait = { .tv_sec = 0, .tv_nsec = SETTLE_WAIT_NS };

        misses = worker_misses;
        nanosleep(&wait, NULL);
        delay_ns = 0;
        round_run();
    }
}

// The runtime's tick runs the models' time_advanced once per millisecond
// it sees go by, so a long step in virtual time is walked in ticks: a
// model with a timed operation finishes it at the tick it would have.
void mclw_host_advance(uint64_t ns) {
    uint64_t now = atomic_load(&virtual_ns);

    delay_ns = 0;
    while (now < ns) {
        uint64_t step = ns - now > WORKER_TICK_US * 1000u ? WORKER_TICK_US * 1000u : ns - now;

        now += step;
        atomic_store(&virtual_ns, now);
        round_settled();
        delay_ns = 0;
    }
    mclw_host_drain_work();
    // What the round and the work delayed went with their own events; the
    // request's events start from its time.
    delay_ns = 0;
}

struct call_context {
    void (*fn)(void *ctx);
    void *ctx;
    bool done;
};

static void call_trampoline(struct mclw_work *work) {
    struct call_context *c = work->ctx;

    c->fn(c->ctx);
    pthread_mutex_lock(&call_mutex);
    c->done = true;
    pthread_cond_broadcast(&call_done);
    pthread_mutex_unlock(&call_mutex);
}

void mclw_hal_work_call(void (*fn)(void *ctx), void *ctx) {
    if (mclw_hal_in_worker()) {
        fn(ctx);
        return;
    }
    struct call_context c = { .fn = fn, .ctx = ctx, .done = false };
    struct mclw_work w = { .fn = call_trampoline, .ctx = &c, .pending = false, .next = NULL };

    mclw_hal_work_post(&w);
    pthread_mutex_lock(&call_mutex);
    while (!c.done) {
        pthread_cond_wait(&call_done, &call_mutex);
    }
    pthread_mutex_unlock(&call_mutex);
}

int mclw_hal_poll_register(mclw_hal_poll_t fn, void *ctx) {
    uint32_t state = mclw_hal_critical_enter();

    if (poll_count >= MCLW_WORKER_POLLS_MAX) {
        mclw_hal_critical_exit(state);
        return -ENOSPC;
    }
    polls[poll_count].fn = fn;
    polls[poll_count].ctx = ctx;
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
            poll_count = n - 1u;
            return;
        }
    }
}

void mclw_hal_poll_unregister(mclw_hal_poll_t fn, void *ctx) {
    struct poll_removal r = { .fn = fn, .ctx = ctx };

    mclw_hal_work_call(poll_remove, &r);
}

void mclw_hal_worker_stats(struct mclw_worker_stats *out, bool reset) {
    size_t n = poll_count;

    out->rounds = stats.rounds;
    out->max_round_gap_us = stats.max_round_gap_us;
    out->max_work_per_round = stats.max_work_per_round;
    out->lock_misses = stats.lock_misses;
    out->poll_count = (uint32_t)n;
    for (size_t i = 0; i < MCLW_WORKER_POLLS_MAX; i++) {
        out->poll_fn[i] = i < n ? (uintptr_t)polls[i].fn : 0u;
        out->poll_max_us[i] = i < n ? stats.poll_max_us[i] : 0u;
    }
    if (reset) {
        stats.max_round_gap_us = 0;
        stats.max_work_per_round = 0;
        stats.lock_misses = 0;
        memset(stats.poll_max_us, 0, sizeof(stats.poll_max_us));
    }
}

void mclw_host_drain_wake(int fd) {
    uint8_t buf[64];

    while (read(fd, buf, sizeof(buf)) > 0) {
    }
}

static void *worker_main(void *arg) {
    const char *path = arg;

    if (path != NULL) {
        for (;;) {
            mclw_host_bridge_run(path, wake_pipe[0]);
        }
    }
    for (;;) {
        struct pollfd p = { .fd = wake_pipe[0], .events = POLLIN, .revents = 0 };

        (void)poll(&p, 1, (int)(WORKER_TICK_US / 1000u));
        mclw_host_drain_wake(wake_pipe[0]);
        round_run();
    }
    return NULL;
}

// Before MicroPython's main: the locks, the runtime, and the worker.
__attribute__((constructor)) static void host_init(void) {
    pthread_mutexattr_t attr;
    const char *path = getenv("METALCLAW_BRIDGE");

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&critical_mutex, &attr);
    pthread_mutex_init(&runtime_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (pipe(wake_pipe) != 0) {
        perror("metalclaw: pipe");
        exit(1);
    }
    set_nonblocking(wake_pipe[0]);
    set_nonblocking(wake_pipe[1]);
    bridged = path != NULL && path[0] != '\0';
    start_ns = monotonic_ns();
    atomic_store(&virtual_ns, 0);
    mclw_runtime_init();
    if (pthread_create(&worker_thread, NULL, worker_main, bridged ? (void *)path : NULL) != 0) {
        perror("metalclaw: worker");
        exit(1);
    }
}
