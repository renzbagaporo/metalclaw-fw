// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/edges.h"
#include "metalclaw/hal/wire.h"

#define READ_BATCH 16u
// An entry is read at the first poll after its edge, about a millisecond
// apart, and the poll itself can come late.
#define POLL_SLACK_US 2000u

void mclw_edges_init(struct mclw_edges *e) {
    memset(e, 0, sizeof(*e));
    e->level = -1;
}

void mclw_edges_take(struct mclw_edges *e, uint32_t entry, uint64_t now_us) {
    bool high = (entry & MCLW_WIRE_MARK) != 0u;
    uint32_t ns = entry & ~MCLW_WIRE_MARK;

    e->last_edge_us = now_us;
    e->any_edge = true;
    if (high) {
        // A fall ended this high.
        e->falling++;
        e->high_ns = ns;
        e->pending_high = ns != MCLW_WIRE_LONG;
        e->pending_ns = ns;
        e->level = 0;
        return;
    }
    // A rise ended this low: with the high before it, a whole cycle.
    e->rising++;
    e->low_ns = ns;
    e->level = 1;
    if (e->pending_high && ns != MCLW_WIRE_LONG && (uint64_t)e->pending_ns + ns < MCLW_WIRE_LONG) {
        e->pulse_ns = e->pending_ns;
        e->period_ns = e->pending_ns + ns;
        e->periods++;
    }
    e->pending_high = false;
}

// The stats before each read: an overrun the engine reports comes between
// what it handed out before and what it hands out after, and the high
// those would pair with may not be the one just before the next low.
void mclw_edges_poll(struct mclw_edges *e, struct mclw_wire *w, uint64_t now_us) {
    uint32_t entries[READ_BATCH];
    struct mclw_wire_stats stats;
    size_t n;

    do {
        mclw_hal_wire_stats(w, &stats);
        if (stats.overruns != e->overruns_seen) {
            e->missed += stats.overruns - e->overruns_seen;
            e->overruns_seen = stats.overruns;
            e->pending_high = false;
        }
        n = mclw_hal_wire_read(w, entries, READ_BATCH);
        for (size_t i = 0; i < n; i++) {
            mclw_edges_take(e, entries[i], now_us);
        }
    } while (n > 0u);
    int level = mclw_hal_wire_level(w, 0);
    if (level >= 0) {
        e->level = (int8_t)level;
    }
}

bool mclw_edges_still(const struct mclw_edges *e, uint64_t now_us, uint32_t idle_us) {
    uint64_t hold = e->periods != 0u ? 2u * ((uint64_t)e->period_ns / 1000u) : idle_us;

    if (!e->any_edge) {
        return true;
    }
    return now_us - e->last_edge_us > hold + POLL_SLACK_US;
}

uint32_t mclw_edges_duty_ppm(const struct mclw_edges *e, uint64_t now_us, uint32_t idle_us) {
    if (mclw_edges_still(e, now_us, idle_us) || e->periods == 0u || e->period_ns == 0u) {
        return e->level == 1 ? 1000000u : 0u;
    }
    return (uint32_t)(((uint64_t)e->pulse_ns * 1000000u + e->period_ns / 2u) / e->period_ns);
}
