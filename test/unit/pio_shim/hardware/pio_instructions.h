// SPDX-License-Identifier: Apache-2.0 OR MIT
// The pico-sdk's PIO instruction encoders that core/ports/rp2/hal/
// wire_programs.c uses, for a host build: the same names and arguments,
// written from the RP2040 datasheet's instruction encoding table (3.4).

#ifndef MCLW_TEST_PIO_INSTRUCTIONS_H
#define MCLW_TEST_PIO_INSTRUCTIONS_H

#include <stdbool.h>

typedef unsigned int uint;

enum pio_src_dest {
    pio_pins = 0u,
    pio_x = 1u,
    pio_y = 2u,
    pio_null = 3u,
    pio_pindirs = 4u,
    pio_status = 5u,
    pio_pc = 5u,
    pio_isr = 6u,
    pio_osr = 7u,
};

static inline uint pio_encode_delay(uint cycles) {
    return cycles << 8u;
}

static inline uint pio_encode_sideset(uint sideset_bit_count, uint value) {
    return value << (13u - sideset_bit_count);
}

static inline uint pio_encode_jmp(uint addr) {
    return 0x0000u | addr;
}

static inline uint pio_encode_jmp_not_x(uint addr) {
    return 0x0000u | 1u << 5 | addr;
}

static inline uint pio_encode_jmp_x_dec(uint addr) {
    return 0x0000u | 2u << 5 | addr;
}

static inline uint pio_encode_jmp_not_y(uint addr) {
    return 0x0000u | 3u << 5 | addr;
}

static inline uint pio_encode_jmp_y_dec(uint addr) {
    return 0x0000u | 4u << 5 | addr;
}

static inline uint pio_encode_jmp_pin(uint addr) {
    return 0x0000u | 6u << 5 | addr;
}

static inline uint pio_encode_wait_pin(bool polarity, uint pin) {
    return 0x2000u | (polarity ? 1u : 0u) << 7 | 1u << 5 | pin;
}

static inline uint pio_encode_in(enum pio_src_dest src, uint count) {
    return 0x4000u | ((uint)src & 7u) << 5 | (count & 0x1fu);
}

static inline uint pio_encode_out(enum pio_src_dest dest, uint count) {
    return 0x6000u | ((uint)dest & 7u) << 5 | (count & 0x1fu);
}

static inline uint pio_encode_push(bool if_full, bool block) {
    return 0x8000u | (if_full ? 1u : 0u) << 6 | (block ? 1u : 0u) << 5;
}

static inline uint pio_encode_pull(bool if_empty, bool block) {
    return 0x8080u | (if_empty ? 1u : 0u) << 6 | (block ? 1u : 0u) << 5;
}

static inline uint pio_encode_mov(enum pio_src_dest dest, enum pio_src_dest src) {
    return 0xa000u | ((uint)dest & 7u) << 5 | ((uint)src & 7u);
}

static inline uint pio_encode_mov_not(enum pio_src_dest dest, enum pio_src_dest src) {
    return 0xa000u | ((uint)dest & 7u) << 5 | 1u << 3 | ((uint)src & 7u);
}

static inline uint pio_encode_set(enum pio_src_dest dest, uint value) {
    return 0xe000u | ((uint)dest & 7u) << 5 | value;
}

#endif
