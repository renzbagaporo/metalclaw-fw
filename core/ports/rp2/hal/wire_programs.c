// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio_instructions.h"

#include "wire_programs.h"

struct builder {
    uint16_t *words;
    uint8_t n;
    uint8_t side_bits;
};

uint32_t wire_to_cycles(uint32_t ns, uint32_t hz) {
    return (uint32_t)(((uint64_t)ns * hz + 500000000u) / 1000000000u);
}

uint32_t wire_to_ns(uint32_t cycles, uint32_t hz) {
    return (uint32_t)(((uint64_t)cycles * 1000000000u + hz / 2u) / hz);
}

static uint8_t emit(struct builder *b, uint32_t instruction, uint32_t side, uint32_t delay) {
    uint32_t max_delay = b->side_bits != 0u ? 15u : 31u;
    uint32_t word = instruction | pio_encode_delay(delay < max_delay ? delay : max_delay);

    if (b->side_bits != 0u) {
        word |= pio_encode_sideset(b->side_bits, side);
    }
    b->words[b->n] = (uint16_t)word;
    return b->n++;
}

// Two instructions that take exactly `n` cycles, two at least: SET X, then
// a pass of JMP X-- for each count.
static void emit_wait(struct builder *b, uint32_t n, uint32_t side) {
    uint32_t max_delay = b->side_bits != 0u ? 15u : 31u;
    uint32_t rest = (n < 2u ? 2u : n) - 1u;
    uint32_t per = (rest + 31u) / 32u;
    uint32_t passes;
    uint32_t first;
    uint8_t at = b->n;

    if (per > max_delay + 1u) {
        per = max_delay + 1u;
    }
    passes = rest / per;
    if (passes > 32u) {
        passes = 32u;
    }
    first = rest - passes * per;
    emit(b, pio_encode_set(pio_x, passes - 1u), side, first);
    emit(b, pio_encode_jmp_x_dec((uint)at + 1u), side, per - 1u);
}

static uint32_t span(uint32_t from, uint32_t to) {
    return to > from + 2u ? to - from : 2u;
}

// A high pulse is checked at the narrowest 0 the datasheet allows, read
// halfway between the widest 0 and the narrowest 1, and checked again at
// the widest 1; the program starts timing a cycle after it sees the rise.
void wire_encode_pulse_bits(struct wire_program *p, const struct mclw_wire_facts *f, uint32_t hz) {
    struct builder b = { p->words, 0, 0 };
    uint32_t check = wire_to_cycles(f->pulse_bits.zero_min_ns, hz);
    uint32_t sample = wire_to_cycles((f->pulse_bits.zero_max_ns + f->pulse_bits.one_min_ns) / 2u, hz);
    uint32_t widest = wire_to_cycles(f->pulse_bits.one_max_ns, hz);

    emit(&b, pio_encode_wait_pin(true, 0), 0, 0);
    emit_wait(&b, span(1u, check), 0);
    emit(&b, pio_encode_jmp_pin(PB_SAMPLE_WAIT), 0, 0);
    emit(&b, pio_encode_mov_not(pio_isr, pio_null), 0, 0);
    emit(&b, pio_encode_in(pio_null, 1), 0, 0);
    emit(&b, pio_encode_push(false, false), 0, 0);
    emit(&b, pio_encode_wait_pin(false, 0), 0, 0);
    emit(&b, pio_encode_mov(pio_x, pio_y), 0, 0);
    emit(&b, pio_encode_jmp_pin(PB_BAD_LOW), 0, 0);
    emit(&b, pio_encode_jmp_x_dec(PB_BAD_LOOP), 0, 0);
    emit(&b, pio_encode_jmp(PB_END), 0, 0);
    emit_wait(&b, span(check + 1u, sample), 0);
    emit(&b, pio_encode_in(pio_pins, 1), 0, 0);
    emit(&b, pio_encode_jmp_pin(PB_ONE), 0, 0);
    emit(&b, pio_encode_mov(pio_x, pio_y), 0, 0);
    emit(&b, pio_encode_jmp_pin(PB_RISE), 0, 0);
    emit(&b, pio_encode_jmp_x_dec(PB_LOW_LOOP), 0, 0);
    emit(&b, pio_encode_mov_not(pio_isr, pio_null), 0, 0);
    emit(&b, pio_encode_push(false, false), 0, 0);
    emit(&b, pio_encode_jmp(PB_IDLE), 0, 0);
    emit_wait(&b, span(sample + 2u, widest), 0);
    emit(&b, pio_encode_jmp_pin(PB_BAD), 0, 0);
    emit(&b, pio_encode_jmp(PB_LOW), 0, 0);
    p->length = b.n;
    p->side_bits = 0;
}

void wire_encode_clocked_in(struct wire_program *p, uint8_t clock_index) {
    p->words[0] = (uint16_t)pio_encode_wait_pin(false, clock_index);
    p->words[1] = (uint16_t)pio_encode_wait_pin(true, clock_index);
    p->words[2] = (uint16_t)pio_encode_in(pio_pins, 1);
    p->length = 3;
    p->side_bits = 0;
}

// IN base is the clock, so a sample's bit 0 is the clock's level, 1 when
// the poll saw it rise; the latch's sample has its bit 0 cleared, since a
// clock edge may come with the latch. The pins are read once per sample,
// so the clock and the data come from the same instant, at most a poll
// pass (4 cycles) after the edge.
void wire_encode_latched_data(struct wire_program *p, uint8_t latch_index) {
    struct builder b = { p->words, 0, 0 };

    emit(&b, pio_encode_wait_pin(true, latch_index), 0, 0);
    emit(&b, pio_encode_wait_pin(false, latch_index), 0, 0);
    emit(&b, pio_encode_wait_pin(false, 0), 0, 0);
    emit(&b, pio_encode_jmp_pin(LD_LATCH), 0, 0);
    emit(&b, pio_encode_mov(pio_osr, pio_pins), 0, 0);
    emit(&b, pio_encode_out(pio_x, 1), 0, 0);
    emit(&b, pio_encode_jmp_not_x(LD_POLL), 0, 0);
    emit(&b, pio_encode_in(pio_x, 1), 0, 0);
    emit(&b, pio_encode_in(pio_osr, 31), 0, 0);
    emit(&b, pio_encode_jmp(LD_TOP), 0, 0);
    emit(&b, pio_encode_mov(pio_osr, pio_pins), 0, 0);
    emit(&b, pio_encode_out(pio_null, 1), 0, 0);
    emit(&b, pio_encode_in(pio_null, 1), 0, 0);
    emit(&b, pio_encode_in(pio_osr, 31), 0, 0);
    emit(&b, pio_encode_jmp(LD_HALT), 0, 0);
    p->length = b.n;
    p->side_bits = 0;
}

// From the second latch to the third, counts the poll passes (5 cycles
// each either way) that find the enable line low.
void wire_encode_latched_enable(struct wire_program *p, uint8_t latch_index) {
    struct builder b = { p->words, 0, 0 };

    emit(&b, pio_encode_wait_pin(true, latch_index), 0, 0);
    emit(&b, pio_encode_wait_pin(false, latch_index), 0, 0);
    emit(&b, pio_encode_wait_pin(true, latch_index), 0, 0);
    emit(&b, pio_encode_wait_pin(false, latch_index), 0, 0);
    emit(&b, pio_encode_mov_not(pio_x, pio_null), 0, 0);
    emit(&b, pio_encode_jmp_pin(LE_DONE), 0, 0);
    emit(&b, pio_encode_mov(pio_osr, pio_pins), 0, 0);
    emit(&b, pio_encode_out(pio_y, 1), 0, 0);
    emit(&b, pio_encode_jmp_not_y(LE_ON), 0, 0);
    emit(&b, pio_encode_jmp(LE_POLL), 0, 0);
    emit(&b, pio_encode_jmp_x_dec(LE_POLL), 0, 0);
    emit(&b, pio_encode_jmp(LE_POLL), 0, 0);
    emit(&b, pio_encode_mov_not(pio_isr, pio_x), 0, 0);
    emit(&b, pio_encode_push(false, true), 0, 0);
    emit(&b, pio_encode_jmp(LE_HALT), 0, 0);
    p->length = b.n;
    p->side_bits = 0;
}

// At 1 us a cycle. Each slot: take the queued bit (1 when none is), hold
// the line low until the sample point for a 0, sample, push; at 55 us,
// inside any slot (tSLOT and tLOW0 are 60 us at least), wait for the line
// to be high, or call a low of 184 us more a reset (longer than tLOW0's
// 120 us, shorter than tRSTL's 480 us): drop any queued bit, push the
// reset word, and answer with presence after the release.
void wire_encode_onewire(struct wire_program *p, const struct mclw_wire_facts *f) {
    struct builder b = { p->words, 0, 1 };
    uint32_t sample = wire_to_cycles(f->onewire.sample_ns, WIRE_PROGRAM_1MHZ);
    uint32_t wait = wire_to_cycles(f->onewire.presence_wait_ns, WIRE_PROGRAM_1MHZ);
    uint32_t presence = wire_to_cycles(f->onewire.presence_ns, WIRE_PROGRAM_1MHZ);

    emit(&b, pio_encode_mov_not(pio_x, pio_null), 0, 0);
    emit(&b, pio_encode_pull(false, false), 0, 0);
    emit(&b, pio_encode_out(pio_y, 1), 0, 0);
    emit(&b, pio_encode_wait_pin(false, 0), 0, 0);
    emit(&b, pio_encode_jmp_not_y(OW_HOLD), 0, 0);
    emit_wait(&b, span(3u, sample), 0);
    emit(&b, pio_encode_jmp(OW_SAMPLE), 0, 0);
    emit_wait(&b, span(2u, sample), 1);
    emit(&b, pio_encode_in(pio_pins, 1), 0, 0);
    emit(&b, pio_encode_push(false, false), 0, 0);
    emit_wait(&b, span(sample + 2u, 55u), 0);
    emit(&b, pio_encode_set(pio_x, 31), 0, 0);
    emit(&b, pio_encode_jmp_pin(OW_SLOT), 0, 0);
    emit(&b, pio_encode_jmp_x_dec(OW_LOW), 0, 2);
    emit(&b, pio_encode_pull(false, false), 0, 0);
    emit(&b, pio_encode_pull(false, false), 0, 0);
    emit(&b, pio_encode_mov_not(pio_isr, pio_null), 0, 0);
    emit(&b, pio_encode_push(false, false), 0, 0);
    emit(&b, pio_encode_wait_pin(true, 0), 0, 0);
    emit_wait(&b, span(1u, wait), 0);
    emit_wait(&b, presence, 1);
    emit(&b, pio_encode_jmp(OW_SLOT), 0, 0);
    p->length = b.n;
    p->side_bits = 1;
}

// Waits for a low of the start's length and its release, then plays the
// words from the TX FIFO: bit 0 drives the line low, the rest counts; a
// count of 0 ends the response. Each word is pulled when it plays, never
// before: an autopull could hold a word of an old waveform in the OSR.
void wire_encode_responder(struct wire_program *p) {
    struct builder b = { p->words, 0, 1 };

    emit(&b, pio_encode_wait_pin(true, 0), 0, 0);
    emit(&b, pio_encode_wait_pin(false, 0), 0, 0);
    emit(&b, pio_encode_mov(pio_x, pio_isr), 0, 0);
    emit(&b, pio_encode_jmp_pin(RS_START), 0, 0);
    emit(&b, pio_encode_jmp_x_dec(RS_LOW), 0, 0);
    emit(&b, pio_encode_wait_pin(true, 0), 0, 0);
    emit(&b, pio_encode_pull(false, true), 0, 0);
    emit(&b, pio_encode_out(pio_y, 1), 0, 0);
    emit(&b, pio_encode_out(pio_x, 31), 0, 0);
    emit(&b, pio_encode_jmp_not_x(RS_START), 0, 0);
    emit(&b, pio_encode_jmp_not_y(RS_HIGH), 0, 0);
    emit(&b, pio_encode_jmp_x_dec(RS_LOW_LOOP), 1, 0);
    emit(&b, pio_encode_jmp(RS_PLAY), 0, 0);
    emit(&b, pio_encode_jmp_x_dec(RS_HIGH), 0, 0);
    emit(&b, pio_encode_jmp(RS_PLAY), 0, 0);
    p->length = b.n;
    p->side_bits = 1;
}

// A low pulse runs its loop once more than its count; a high one also
// spans the instructions around it, thirteen cycles all told.
uint32_t wire_responder_word(uint32_t ns, bool low, uint32_t hz) {
    uint32_t cycles = wire_to_cycles(ns, hz);
    uint32_t overhead = low ? 1u : 13u;
    uint32_t n = cycles > overhead + 1u ? cycles - overhead : 1u;

    return (n << 1) | (low ? 1u : 0u);
}

// Poll passes of five cycles.
uint32_t wire_latched_enabled_ns(uint32_t passes, uint32_t hz) {
    return wire_to_ns(passes * 5u, hz);
}

// A burst's word counts passes of three cycles, a gap's of two, each
// with the few cycles around its loop.
uint32_t wire_envelope_entry(uint32_t word, uint32_t hz) {
    bool mark = (word & 0x80000000u) != 0u;
    uint32_t cycles = mark ? (~word) * 3u + 4u : word * 2u + 5u;

    return (mark ? 0x80000000u : 0u) | ((wire_to_ns(cycles, hz) / 1000u) & 0x7fffffffu);
}

void wire_encode_steps_out(struct wire_program *p, uint8_t count) {
    p->words[0] = (uint16_t)pio_encode_out(pio_pins, count);
    p->words[1] = (uint16_t)pio_encode_mov(pio_x, pio_y);
    p->words[2] = (uint16_t)pio_encode_jmp_x_dec(2);
    p->length = 3;
    p->side_bits = 0;
}

// At 1 us a cycle, side-set on the output, which idles high. A gap counts
// in passes of two cycles; a burst in passes of three, the gap timer
// (OSR, set at open) running whenever the input is low and restarting at
// every high. A burst's word is Y as it stands, bit 31 set; a gap's is its
// complement, bit 31 clear.
void wire_encode_envelope(struct wire_program *p) {
    struct builder b = { p->words, 0, 1 };

    emit(&b, pio_encode_mov_not(pio_y, pio_null), 1, 0);
    emit(&b, pio_encode_jmp_pin(EN_MARK), 1, 0);
    emit(&b, pio_encode_jmp_y_dec(EN_SPACE_LOOP), 1, 0);
    emit(&b, pio_encode_jmp(EN_SPACE_LOOP), 1, 0);
    emit(&b, pio_encode_mov_not(pio_isr, pio_y), 0, 0);
    emit(&b, pio_encode_push(false, false), 0, 0);
    emit(&b, pio_encode_mov_not(pio_y, pio_null), 0, 0);
    emit(&b, pio_encode_mov(pio_x, pio_osr), 0, 0);
    emit(&b, pio_encode_jmp_y_dec(EN_RELOAD + 2u), 0, 0);
    emit(&b, pio_encode_jmp_pin(EN_RELOAD), 0, 0);
    emit(&b, pio_encode_jmp_x_dec(EN_RELOAD + 1u), 0, 0);
    emit(&b, pio_encode_mov(pio_isr, pio_y), 1, 0);
    emit(&b, pio_encode_push(false, false), 1, 0);
    emit(&b, pio_encode_jmp(EN_SPACE), 1, 0);
    p->length = b.n;
    p->side_bits = 1;
}

// At the full harness clock, no side-set. A level is counted in passes of
// two cycles from Y down; at its end the count left goes out, a low's as
// it is (bit 31 clear), a high's complemented (bit 31 set). A level that
// outlasts Y, and the one the program starts in, whose start it did not
// see, go out as 0 (complemented for a high) once they end. Every edge is
// seen by a JMP PIN pass or a WAIT landing on the same cycle as one, so
// the lengths add up to the period exactly: a low is 2n + 6 cycles, a high
// 2n + 4, from the cycle that saw its first edge to the one that saw its
// last. A full FIFO drops the entry (the edge is lost, FDEBUG.RXSTALL says
// so); the levels after it are timed as before.
void wire_encode_edges_in(struct wire_program *p) {
    struct builder b = { p->words, 0, 0 };

    emit(&b, pio_encode_jmp_pin(ED_HIGH_LONG), 0, 0);
    emit(&b, pio_encode_jmp(ED_LOW_LONG), 0, 0);
    emit(&b, pio_encode_mov(pio_x, pio_y), 0, 0);
    emit(&b, pio_encode_jmp_pin(ED_LOW_END), 0, 0);
    emit(&b, pio_encode_jmp_x_dec(ED_LOW_LOOP), 0, 0);
    emit(&b, pio_encode_mov(pio_x, pio_null), 0, 0);
    emit(&b, pio_encode_wait_pin(true, 0), 0, 0);
    emit(&b, pio_encode_mov(pio_isr, pio_x), 0, 0);
    emit(&b, pio_encode_push(false, false), 0, 0);
    emit(&b, pio_encode_mov(pio_x, pio_y), 0, 0);
    emit(&b, pio_encode_jmp_pin(ED_HIGH_CONT), 0, 0);
    emit(&b, pio_encode_jmp(ED_HIGH_END), 0, 0);
    emit(&b, pio_encode_jmp_x_dec(ED_HIGH_LOOP), 0, 0);
    emit(&b, pio_encode_mov(pio_x, pio_null), 0, 0);
    emit(&b, pio_encode_wait_pin(false, 0), 0, 1);
    emit(&b, pio_encode_mov_not(pio_isr, pio_x), 0, 0);
    emit(&b, pio_encode_push(false, false), 0, 0);
    emit(&b, pio_encode_jmp(ED_LOW), 0, 0);
    p->length = b.n;
    p->side_bits = 0;
}

// Passes of two cycles to MCLW_WIRE_LONG ns: under 2^31 at any clock a
// harness runs, so bit 31 tells a high's word from a low's.
uint32_t wire_edges_limit(uint32_t hz) {
    uint64_t cycles = ((uint64_t)0x7fffffffu * hz + 500000000u) / 1000000000u;
    uint64_t passes = cycles / 2u;

    return passes < 0x7fffffffu ? (uint32_t)passes : 0x7fffffffu;
}

uint32_t wire_edges_entry(uint32_t word, uint32_t hz, uint32_t limit) {
    bool high = (word & 0x80000000u) != 0u;
    uint32_t left = high ? ~word : word;
    uint32_t mark = high ? 0x80000000u : 0u;

    if (left == 0u || left > limit) {
        return mark | 0x7fffffffu;
    }
    uint64_t cycles = 2u * (uint64_t)(limit - left) + (high ? 4u : 6u);
    uint64_t ns = (cycles * 1000000000u + hz / 2u) / hz;

    return mark | (ns < 0x7fffffffu ? (uint32_t)ns : 0x7fffffffu);
}
