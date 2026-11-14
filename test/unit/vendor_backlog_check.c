// SPDX-License-Identifier: Apache-2.0 OR MIT
// Exercises vendor_backlog.h on the host: order through wrap-around, the
// push that only partly fits, and peek's contiguous runs. Exits non-zero
// on the first failed check, naming it.

#include <stdio.h>
#include <stdlib.h>

#include "vendor_backlog.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("failed: %s (line %d)\n", #cond, __LINE__); \
            exit(1); \
        } \
    } while (0)

static uint8_t pattern[2u * MCLW_VENDOR_BACKLOG_SIZE];

// Takes up to `want` bytes the way the drain does: run by run.
static size_t take(struct mclw_vendor_backlog *b, uint8_t *out, size_t want) {
    size_t got = 0;
    while (got < want && b->count > 0u) {
        const uint8_t *run;
        size_t len = mclw_vendor_backlog_peek(b, &run);
        if (len > want - got) {
            len = want - got;
        }
        for (size_t i = 0; i < len; i++) {
            out[got + i] = run[i];
        }
        mclw_vendor_backlog_drop(b, len);
        got += len;
    }
    return got;
}

int main(void) {
    static struct mclw_vendor_backlog b;
    static uint8_t out[2u * MCLW_VENDOR_BACKLOG_SIZE];

    for (size_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (uint8_t)(i * 7u + 3u);
    }
    mclw_vendor_backlog_clear(&b);

    // A push that fits is taken whole and comes back in order.
    CHECK(mclw_vendor_backlog_push(&b, pattern, 100) == 100);
    CHECK(take(&b, out, 100) == 100);
    for (size_t i = 0; i < 100; i++) {
        CHECK(out[i] == pattern[i]);
    }

    // Past the end of the buffer, order holds and peek gives the run before
    // the wrap first.
    CHECK(mclw_vendor_backlog_push(&b, pattern, MCLW_VENDOR_BACKLOG_SIZE - 10u) == MCLW_VENDOR_BACKLOG_SIZE - 10u);
    const uint8_t *run;
    CHECK(mclw_vendor_backlog_peek(&b, &run) == MCLW_VENDOR_BACKLOG_SIZE - 100u);
    CHECK(take(&b, out, MCLW_VENDOR_BACKLOG_SIZE - 10u) == MCLW_VENDOR_BACKLOG_SIZE - 10u);
    for (size_t i = 0; i < MCLW_VENDOR_BACKLOG_SIZE - 10u; i++) {
        CHECK(out[i] == pattern[i]);
    }
    CHECK(b.count == 0u);

    // Full: a push takes only what fits, and nothing more until a drop.
    CHECK(mclw_vendor_backlog_push(&b, pattern, MCLW_VENDOR_BACKLOG_SIZE - 4u) == MCLW_VENDOR_BACKLOG_SIZE - 4u);
    CHECK(mclw_vendor_backlog_push(&b, pattern + MCLW_VENDOR_BACKLOG_SIZE - 4u, 12) == 4u);
    CHECK(mclw_vendor_backlog_push(&b, pattern, 1) == 0u);
    CHECK(take(&b, out, 8) == 8u);
    CHECK(mclw_vendor_backlog_push(&b, pattern + MCLW_VENDOR_BACKLOG_SIZE, 8) == 8u);
    CHECK(take(&b, out + 8, MCLW_VENDOR_BACKLOG_SIZE) == MCLW_VENDOR_BACKLOG_SIZE);
    for (size_t i = 0; i < MCLW_VENDOR_BACKLOG_SIZE + 8u; i++) {
        CHECK(out[i] == pattern[i]);
    }

    // Clear forgets what waited; a drop past the end empties it.
    CHECK(mclw_vendor_backlog_push(&b, pattern, 5) == 5u);
    mclw_vendor_backlog_drop(&b, 50);
    CHECK(b.count == 0u);
    CHECK(mclw_vendor_backlog_push(&b, pattern, 5) == 5u);
    mclw_vendor_backlog_clear(&b);
    CHECK(b.count == 0u && b.head == 0u);
    puts("ok");
    return 0;
}
