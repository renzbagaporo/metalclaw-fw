// SPDX-License-Identifier: Apache-2.0 OR MIT
// The wire engines' PIO programs, encoded from the facts' times and a
// state machine clock. Only instruction encodings here, so the programs
// build and run on a host too (test/unit/test_wire_engines.py).

#ifndef MCLW_RP2_WIRE_PROGRAMS_H
#define MCLW_RP2_WIRE_PROGRAMS_H

#include <stdbool.h>
#include <stdint.h>

#include "metalclaw/bus.h"

#define WIRE_PROGRAM_WORDS 32u
#define WIRE_PROGRAM_1MHZ 1000000u

struct wire_program {
    uint16_t words[WIRE_PROGRAM_WORDS];
    uint8_t length;
    uint8_t side_bits;
};

// Program layouts: the addresses jumps are encoded against.
enum {
    PB_IDLE = 0,
    PB_RISE = 1,
    PB_BAD = 4,
    PB_BAD_LOW = 7,
    PB_BAD_LOOP = 9,
    PB_SAMPLE_WAIT = 12,
    PB_LOW = 16,
    PB_LOW_LOOP = 17,
    PB_END = 19,
    PB_ONE = 22,
};

enum {
    OW_SLOT = 0,
    OW_HOLD = 8,
    OW_SAMPLE = 10,
    OW_LOW = 15,
};

enum {
    RS_START = 0,
    RS_LOW = 3,
    RS_PLAY = 6,
    RS_LOW_LOOP = 11,
    RS_HIGH = 13,
    // A start in progress or none: the waveform can change under it.
    RS_SAFE_END = 5,
};

enum {
    EN_SPACE = 0,
    EN_SPACE_LOOP = 1,
    EN_MARK = 4,
    EN_RELOAD = 7,
};

enum {
    LD_TOP = 2,
    LD_POLL = 3,
    LD_LATCH = 10,
    LD_HALT = 14,
    LE_POLL = 5,
    LE_ON = 10,
    LE_DONE = 12,
    LE_HALT = 14,
};

uint32_t wire_to_cycles(uint32_t ns, uint32_t hz);
uint32_t wire_to_ns(uint32_t cycles, uint32_t hz);

void wire_encode_pulse_bits(struct wire_program *p, const struct mclw_wire_facts *f, uint32_t hz);
void wire_encode_clocked_in(struct wire_program *p, uint8_t clock_index);
void wire_encode_latched_data(struct wire_program *p, uint8_t latch_index);
void wire_encode_latched_enable(struct wire_program *p, uint8_t latch_index);
void wire_encode_onewire(struct wire_program *p, const struct mclw_wire_facts *f);
void wire_encode_responder(struct wire_program *p);
// A RESPONDER pulse as the TX word the program plays: bit 0 drives the
// line low, the rest counts.
uint32_t wire_responder_word(uint32_t ns, bool low, uint32_t hz);
void wire_encode_steps_out(struct wire_program *p, uint8_t count);
// What LATCHED_IN's enable count and an ENVELOPE word mean, as hal/wire.h
// reports them: ns, and MCLW_WIRE_MARK and us.
uint32_t wire_latched_enabled_ns(uint32_t passes, uint32_t hz);
uint32_t wire_envelope_entry(uint32_t word, uint32_t hz);
void wire_encode_envelope(struct wire_program *p);

#endif
