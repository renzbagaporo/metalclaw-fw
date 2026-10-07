// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_VALUE_H
#define MCLW_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MCLW_VALUE_MAX_COUNT 8

enum mclw_value_type {
    MCLW_TYPE_BOOL,
    MCLW_TYPE_I32,
    MCLW_TYPE_U32,
};

// Every scalar and array property travels as this. The descriptor's type
// and count say how to interpret it; range checks happen in the runtime
// before the model sees it.
struct mclw_value {
    uint8_t count;
    int64_t v[MCLW_VALUE_MAX_COUNT];
};

#endif
