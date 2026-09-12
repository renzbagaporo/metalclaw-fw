// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <string.h>

#include "metalclaw/stream.h"

void metalclaw_stream_init(struct metalclaw_stream *stream, enum metalclaw_stream_direction direction,
    uint16_t length, uint8_t filler) {
    memset(stream, 0, sizeof(*stream));
    stream->direction = direction;
    stream->length = length > METALCLAW_STREAM_FRAME_MAX ? METALCLAW_STREAM_FRAME_MAX : length;
    stream->filler = filler;
    memset(stream->frame, filler, sizeof(stream->frame));
}

void metalclaw_stream_set_on_frame(struct metalclaw_stream *stream, metalclaw_stream_on_frame_t fn, void *ctx) {
    stream->on_frame = fn;
    stream->on_frame_ctx = ctx;
}

void metalclaw_stream_publish(struct metalclaw_stream *stream, const uint8_t *bytes, size_t len) {
    uint8_t next = stream->active ^ 1u;
    if (len > stream->length) {
        len = stream->length;
    }
    memcpy(stream->frame[next], bytes, len);
    memset(stream->frame[next] + len, stream->filler, stream->length - len);
    stream->active = next;
    if (stream->notify != NULL) {
        stream->notify(stream, stream->notify_ctx);
    }
}
