// SPDX-License-Identifier: Apache-2.0 OR MIT
// The NEC code as Vishay's "Data Formats for IR Remote Control" (document
// 80071, Rev. 2.3, figure 5) gives it, read from the bursts and gaps an IR
// receiver's output shows.

#ifndef MCLW_DEVICE_NEC_H
#define MCLW_DEVICE_NEC_H

#include <stdbool.h>
#include <stdint.h>

struct nec_decoder {
    uint8_t state;
    uint8_t bits;
    uint32_t word;
    uint32_t frames;
    uint32_t repeats;
    uint32_t errors;
    uint32_t last;
};

void nec_init(struct nec_decoder *d);
// One burst (mark) or gap and its length in us. True when it completed a
// frame, which is then in `last`, its first bit in bit 0.
bool nec_take(struct nec_decoder *d, bool mark, uint32_t us);

#endif
