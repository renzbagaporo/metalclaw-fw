// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <string.h>

#include "metalclaw/sink.h"

void mclw_sink_init(struct mclw_sink *sink, mclw_sink_on_bytes_t on_bytes, void *ctx) {
    memset(sink, 0, sizeof(*sink));
    sink->on_bytes = on_bytes;
    sink->ctx = ctx;
    sink->read_byte = 0xff;
}
