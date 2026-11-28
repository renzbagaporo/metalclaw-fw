// SPDX-License-Identifier: Apache-2.0 OR MIT
// What an EDGES_IN wire's entries (hal/wire.h) add up to, for a model that
// watches a pin the DUT drives: its level, its edges counted, and the
// period and high time of the last whole cycle, as a PWM output has them.
// A model polls the wire into it from its poll.

#ifndef MCLW_EDGES_H
#define MCLW_EDGES_H

#include <stdbool.h>
#include <stdint.h>

struct mclw_wire;

struct mclw_edges {
    // The level now: 0, 1, or -1 while the port cannot tell.
    int8_t level;
    uint32_t rising;
    uint32_t falling;
    // Overruns the engine reported: each lost one edge or more.
    uint32_t missed;
    // The clock (mclw_now_us) at the poll that read the latest edge, and
    // whether there has been one.
    uint64_t last_edge_us;
    bool any_edge;
    // The last whole cycle, a high and the low after it (rise to rise),
    // with no edge lost between: its length and its high time, in ns, and
    // how many such cycles there have been.
    uint32_t period_ns;
    uint32_t pulse_ns;
    uint32_t periods;
    // The latest high and low, each as it ended, in ns.
    uint32_t high_ns;
    uint32_t low_ns;
    // A high that ended, whose low has not.
    bool pending_high;
    uint32_t pending_ns;
    uint32_t overruns_seen;
};

void mclw_edges_init(struct mclw_edges *e);
// Takes one entry, read at the clock `now_us`.
void mclw_edges_take(struct mclw_edges *e, uint32_t entry, uint64_t now_us);
// Reads everything the wire has, its overruns and its level.
void mclw_edges_poll(struct mclw_edges *e, struct mclw_wire *w, uint64_t now_us);
// Whether the line has held its level since longer than two of its last
// cycles (or `idle_us`, with none yet) and the poll's own lag: a PWM at
// 0 % or 100 % duty, or a line that stopped.
bool mclw_edges_still(const struct mclw_edges *e, uint64_t now_us, uint32_t idle_us);
// The duty of the last cycle in parts per million, or the level's (0 or
// 1000000) while the line is still.
uint32_t mclw_edges_duty_ppm(const struct mclw_edges *e, uint64_t now_us, uint32_t idle_us);

#endif
