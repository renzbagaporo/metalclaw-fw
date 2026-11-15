// SPDX-License-Identifier: Apache-2.0 OR MIT
// A wire: a few pins whose timing the part defines, run by one of the
// port's wire engines. The model names the engine and gives it the
// datasheet's times in its binding (struct mclw_wire_facts, bus.h); the
// engine meets every edge in hardware. A model reads what an engine
// captured, or queues what it puts out, from its poll on the worker; the
// 1-Wire engine answers each slot itself from an image (onewire.h).
//
//   PULSE_BITS_IN  (data): a single-wire LED line. Each high pulse is a bit,
//                  told apart by its width; a long low ends the frame.
//                  read(): one entry per byte, MSB first on the wire, and
//                  MCLW_WIRE_END where a frame ended, MCLW_WIRE_BAD where a
//                  pulse fell outside both windows (that frame is spoiled).
//   CLOCKED_IN     (clock, data): data sampled at each rising clock edge.
//                  read(): words of 32 bits, the first received in bit 31.
//   LATCHED_IN     (clock, latch, enable): a shift-register panel. capture()
//                  arms one look: from a latch pulse on, every pin in reach
//                  is sampled at each rising clock edge and at the next
//                  latch pulse, and the time the enable line (active low)
//                  is asserted from that latch to the one after is timed.
//                  read() then returns the samples, the latch's last, each
//                  with bit k for pin sample_bit(k) (the clock's bit is 1
//                  in a clock sample, and cleared in the latch's), and a
//                  last word, the enabled time in ns.
//   PINS           (pin, ...): pins another binding's engine samples; no
//                  engine of its own.
//   ONEWIRE        (dq): a 1-Wire slave. Reset and presence, and every
//                  slot, in hardware; the network layer and the image's
//                  function commands in core (onewire.h), per slot.
//   RESPONDER      (line): an open-drain line that answers a host's start
//                  (a low at least start_min_ns long, then its release)
//                  with the waveform respond() set.
//   STEPS_OUT      (first, ...): consecutive pins stepped through states
//                  the model writes, one each period.
//   ENVELOPE       (in, out): drives `out` low while a carrier is on `in`,
//                  as an IR receiver module does, and times each burst
//                  and gap. read(): MCLW_WIRE_MARK and a duration in us.

#ifndef MCLW_HAL_WIRE_H
#define MCLW_HAL_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"

struct mclw_wire;
struct mclw_onewire;

#define MCLW_WIRE_PINS_MAX 4u

// PULSE_BITS_IN entries.
#define MCLW_WIRE_END 0x100u
#define MCLW_WIRE_BAD 0x200u
// ENVELOPE entries: set for a burst, clear for a gap; the rest is us.
#define MCLW_WIRE_MARK 0x80000000u
// RESPONDER pulses: set to drive the line low, clear to release it; the
// rest is ns.
#define MCLW_WIRE_LOW 0x80000000u

struct mclw_wire_stats {
    // Entries or words the engine delivered.
    uint32_t words;
    // Frames, captures, responses or 1-Wire resets, as the engine has them.
    uint32_t frames;
    // Spoiled frames, captures that overflowed.
    uint32_t errors;
    // What the engine captured that was overwritten before it was read.
    uint32_t overruns;
};

// Claims `count` pins for `owner` and loads the facts' engine. Input
// engines capture from here on; LATCHED_IN waits for capture(), ONEWIRE
// for an image, RESPONDER for a waveform, and STEPS_OUT holds the first
// state (all pins low) until states are written.
struct mclw_wire *mclw_hal_wire_open(const uint8_t *pins, uint8_t count, const struct mclw_wire_facts *facts,
    const void *owner, int *err);
void mclw_hal_wire_close(struct mclw_wire *w);

void mclw_hal_wire_stats(const struct mclw_wire *w, struct mclw_wire_stats *out);
uint8_t mclw_hal_wire_pin(const struct mclw_wire *w, uint8_t index);
// LATCHED_IN: the bit `pin` takes in a sample, or -1 when the engine cannot
// sample it.
int mclw_hal_wire_sample_bit(const struct mclw_wire *w, uint8_t pin);

// PULSE_BITS_IN, CLOCKED_IN, LATCHED_IN, ENVELOPE: what arrived since the
// last call, at most `max`; a LATCHED_IN capture comes whole, once it is
// complete, and needs `max` of the engine's capture_words() at least.
size_t mclw_hal_wire_read(struct mclw_wire *w, uint32_t *out, size_t max);
int mclw_hal_wire_capture(struct mclw_wire *w);
size_t mclw_hal_wire_capture_words(const struct mclw_wire *w);

// RESPONDER: the waveform for every start from now on, taken over between
// responses; the line is released after its last pulse. Responses played.
int mclw_hal_wire_respond(struct mclw_wire *w, const uint32_t *pulses, size_t count);
uint32_t mclw_hal_wire_responses(struct mclw_wire *w);

// STEPS_OUT: one state each `ns`; -EBUSY while states are still queued.
// write() takes whole words of 32 / pins states, the first in the low
// bits, and returns how many words fitted.
int mclw_hal_wire_set_period(struct mclw_wire *w, uint32_t ns);
size_t mclw_hal_wire_write(struct mclw_wire *w, const uint32_t *words, size_t count);
bool mclw_hal_wire_idle(struct mclw_wire *w);

// ONEWIRE: the image must outlive the arm; the engine edits its bus state
// and reads its memory from interrupt context, under the critical section.
int mclw_hal_wire_onewire_arm(struct mclw_wire *w, struct mclw_onewire *image);

#endif
