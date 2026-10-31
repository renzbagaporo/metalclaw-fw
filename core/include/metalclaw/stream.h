// SPDX-License-Identifier: Apache-2.0 OR MIT
// A fixed-length frame a bus-target engine serves or captures per
// chip-select window, for parts with no register map: a converter that
// clocks out a sample, a DAC that clocks one in.
//
// OUT: the part drives the bus. The model publishes the next frame with
// mclw_stream_publish(); the engine serves it from the next window on
// and repeats it until the next publish. Bytes past the frame are `filler`.
// Every window carries one whole frame. While windows arrive back to back
// (one ended less than 2 ms ago) the engine does not re-arm under them: a
// frame published then is served from the second window after the publish at
// the latest, and the window served before it is counted in `stale_windows`.
// IN: the part receives. The engine captures each window and hands it to
// `on_frame` in interrupt context: no allocation, no HAL calls, keep it short.

#ifndef MCLW_STREAM_H
#define MCLW_STREAM_H

#include <stddef.h>
#include <stdint.h>

#define MCLW_STREAM_FRAME_MAX 64

enum mclw_stream_direction {
    MCLW_STREAM_OUT,
    MCLW_STREAM_IN,
};

struct mclw_stream;

typedef void (*mclw_stream_on_frame_t)(struct mclw_stream *stream, const uint8_t *bytes,
    size_t len, void *ctx);
typedef void (*mclw_stream_notify_t)(struct mclw_stream *stream, void *ctx);

struct mclw_stream {
    enum mclw_stream_direction direction;
    uint16_t length;
    uint8_t filler;
    // Two frames; `active` is the one the engine serves. Only the engine
    // and mclw_stream_publish() touch it.
    volatile uint8_t active;
    uint8_t frame[2][MCLW_STREAM_FRAME_MAX];
    mclw_stream_on_frame_t on_frame;
    void *on_frame_ctx;
    // Set by the engine at arm; called after each publish.
    mclw_stream_notify_t notify;
    void *notify_ctx;
};

void mclw_stream_init(struct mclw_stream *stream, enum mclw_stream_direction direction,
    uint16_t length, uint8_t filler);
void mclw_stream_set_on_frame(struct mclw_stream *stream, mclw_stream_on_frame_t fn, void *ctx);

// OUT only. Copies `len` bytes (short frames are padded with filler) into
// the inactive frame and swaps. Safe from thread context while armed.
void mclw_stream_publish(struct mclw_stream *stream, const uint8_t *bytes, size_t len);

#endif
