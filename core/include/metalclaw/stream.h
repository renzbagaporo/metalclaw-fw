// SPDX-License-Identifier: Apache-2.0 OR MIT
// A fixed-length frame a bus-target engine serves or captures per
// chip-select window, for parts with no register map: a converter that
// clocks out a sample, a DAC that clocks one in.
//
// OUT: the part drives the bus. The model publishes the next frame with
// metalclaw_stream_publish(); the engine serves it from the next window on
// and repeats it until the next publish. Bytes past the frame are `filler`.
// IN: the part receives. The engine captures each window and hands it to
// `on_frame` in interrupt context: no allocation, no HAL calls, keep it short.

#ifndef METALCLAW_STREAM_H
#define METALCLAW_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define METALCLAW_STREAM_FRAME_MAX 64

enum metalclaw_stream_direction {
    METALCLAW_STREAM_OUT,
    METALCLAW_STREAM_IN,
};

struct metalclaw_stream;

typedef void (*metalclaw_stream_on_frame_t)(struct metalclaw_stream *stream, const uint8_t *bytes,
    size_t len, void *ctx);
typedef void (*metalclaw_stream_notify_t)(struct metalclaw_stream *stream, void *ctx);

struct metalclaw_stream {
    enum metalclaw_stream_direction direction;
    uint16_t length;
    uint8_t filler;
    // Two frames; `active` is the one the engine serves. Only the engine
    // and metalclaw_stream_publish() touch it.
    volatile uint8_t active;
    uint8_t frame[2][METALCLAW_STREAM_FRAME_MAX];
    metalclaw_stream_on_frame_t on_frame;
    void *on_frame_ctx;
    // Set by the engine at arm; called after each publish.
    metalclaw_stream_notify_t notify;
    void *notify_ctx;
};

void metalclaw_stream_init(struct metalclaw_stream *stream, enum metalclaw_stream_direction direction,
    uint16_t length, uint8_t filler);
void metalclaw_stream_set_on_frame(struct metalclaw_stream *stream, metalclaw_stream_on_frame_t fn, void *ctx);

// OUT only. Copies `len` bytes (short frames are padded with filler) into
// the inactive frame and swaps. Safe from thread context while armed.
void metalclaw_stream_publish(struct metalclaw_stream *stream, const uint8_t *bytes, size_t len);
const uint8_t *metalclaw_stream_current(const struct metalclaw_stream *stream);

#endif
