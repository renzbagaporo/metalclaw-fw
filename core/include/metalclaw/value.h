// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef METALCLAW_VALUE_H
#define METALCLAW_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define METALCLAW_VALUE_MAX_COUNT 8

enum metalclaw_value_type {
    METALCLAW_TYPE_BOOL,
    METALCLAW_TYPE_I32,
    METALCLAW_TYPE_U32,
};

// Every scalar and array property travels as this. The descriptor's type
// and count say how to interpret it; range checks happen in the runtime
// before the model sees it.
struct metalclaw_value {
    uint8_t count;
    int64_t v[METALCLAW_VALUE_MAX_COUNT];
};

#endif
