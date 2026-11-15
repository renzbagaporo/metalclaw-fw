// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "nec.h"

enum {
    NEC_IDLE,
    NEC_LEADER,
    NEC_REPEAT,
    NEC_BIT_MARK,
    NEC_BIT_SPACE,
};

// Figure 5: a 9 ms leader burst, then 4.5 ms before the data or 2.25 ms
// before a repeat's burst; each bit a 562.5 us burst, the next one 1.125 ms
// later for a 0 and 2.25 ms later for a 1; the last bit closed by a burst.
// A receiver's output stretches bursts and shrinks gaps by up to five
// carrier periods (TSOP382, 82491, tpo), hence the windows.
#define LEADER_MIN_US 7000u
#define LEADER_MAX_US 11000u
#define DATA_GAP_MIN_US 3500u
#define DATA_GAP_MAX_US 5500u
#define REPEAT_GAP_MIN_US 1700u
#define REPEAT_GAP_MAX_US 2800u
#define BURST_MIN_US 300u
#define BURST_MAX_US 900u
#define ZERO_GAP_MAX_US 900u
#define ONE_GAP_MIN_US 1300u
#define ONE_GAP_MAX_US 2100u
#define FRAME_BITS 32u

static bool within(uint32_t us, uint32_t lo, uint32_t hi) {
    return us >= lo && us <= hi;
}

void nec_init(struct nec_decoder *d) {
    d->state = NEC_IDLE;
    d->bits = 0;
    d->word = 0;
    d->frames = 0;
    d->repeats = 0;
    d->errors = 0;
    d->last = 0;
}

static void fail(struct nec_decoder *d, bool mark, uint32_t us) {
    d->errors++;
    d->state = mark && within(us, LEADER_MIN_US, LEADER_MAX_US) ? NEC_LEADER : NEC_IDLE;
}

bool nec_take(struct nec_decoder *d, bool mark, uint32_t us) {
    switch (d->state) {
        case NEC_IDLE:
            if (mark && within(us, LEADER_MIN_US, LEADER_MAX_US)) {
                d->state = NEC_LEADER;
            }
            return false;
        case NEC_LEADER:
            if (!mark && within(us, DATA_GAP_MIN_US, DATA_GAP_MAX_US)) {
                d->state = NEC_BIT_MARK;
                d->bits = 0;
                d->word = 0;
            } else if (!mark && within(us, REPEAT_GAP_MIN_US, REPEAT_GAP_MAX_US)) {
                d->state = NEC_REPEAT;
            } else {
                fail(d, mark, us);
            }
            return false;
        case NEC_REPEAT:
            if (mark && within(us, BURST_MIN_US, BURST_MAX_US)) {
                d->repeats++;
                d->state = NEC_IDLE;
            } else {
                fail(d, mark, us);
            }
            return false;
        case NEC_BIT_MARK:
            if (!mark || !within(us, BURST_MIN_US, BURST_MAX_US)) {
                fail(d, mark, us);
                return false;
            }
            if (d->bits == FRAME_BITS) {
                d->frames++;
                d->last = d->word;
                d->state = NEC_IDLE;
                return true;
            }
            d->state = NEC_BIT_SPACE;
            return false;
        case NEC_BIT_SPACE:
            if (!mark && within(us, BURST_MIN_US, ZERO_GAP_MAX_US)) {
                d->bits++;
            } else if (!mark && within(us, ONE_GAP_MIN_US, ONE_GAP_MAX_US)) {
                d->word |= 1u << d->bits;
                d->bits++;
            } else {
                fail(d, mark, us);
                return false;
            }
            d->state = NEC_BIT_MARK;
            return false;
        default:
            d->state = NEC_IDLE;
            return false;
    }
}
