// SPDX-License-Identifier: Apache-2.0 OR MIT
// What a bus-target engine captures for a part that only listens: a display
// controller, an LED driver, a port expander. Such a part has no register
// map to serve; it takes a stream of commands and data whose meaning
// depends on what came before. The engine captures every byte the host
// writes, in order, and hands them to `on_bytes` on the worker under the
// runtime lock, a batch at a time, so the model decodes them, edits its
// state and traces as it likes.
//
// An entry is the byte in bits 7..0, and:
//   MCLW_SINK_FIRST  the first byte of a window: the first after chip
//                    select fell (SPI), or after START and the address
//                    (I2C)
//   MCLW_SINK_LINE   the part's side line, a data/command select, was high
//                    as the byte's last bit was clocked (SPI with a line)
// `ended` says the window the last entry belongs to is over: chip select
// is high again, or a STOP or repeated START followed. A batch may stop in
// the middle of a window, and a later batch may carry no entries, only the
// news that the window has ended.
//
// An I2C read of the part is answered with `read_byte` for every byte; it
// is one byte, so the model may store it from any context.

#ifndef MCLW_SINK_H
#define MCLW_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MCLW_SINK_FIRST 0x100u
#define MCLW_SINK_LINE 0x200u
#define MCLW_SINK_BYTE(entry) ((uint8_t)((entry) & 0xffu))

struct mclw_sink;

typedef void (*mclw_sink_on_bytes_t)(struct mclw_sink *sink, const uint16_t *entries, size_t count,
    bool ended, void *ctx);

struct mclw_sink {
    mclw_sink_on_bytes_t on_bytes;
    void *ctx;
    volatile uint8_t read_byte;
};

void mclw_sink_init(struct mclw_sink *sink, mclw_sink_on_bytes_t on_bytes, void *ctx);

#endif
