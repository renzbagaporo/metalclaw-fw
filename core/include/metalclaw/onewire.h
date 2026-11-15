// SPDX-License-Identifier: Apache-2.0 OR MIT
// A 1-Wire slave's image. The engine (hal/wire.h, ONEWIRE) times reset,
// presence and every slot in hardware and calls this layer once a slot,
// under the critical section, with the level the part sampled; it returns
// what the part does in the next slot. Here are the ROM commands every
// 1-Wire part answers (Maxim DS18B20 042208, "ROM COMMANDS" and figure 11:
// SEARCH ROM F0h, READ ROM 33h, MATCH ROM 55h, SKIP ROM CCh, ALARM SEARCH
// ECh) and, once the part is selected, the model's function commands, each
// answered from the image with no model call:
//
//   tx_length   bytes from memory[tx_offset] go out, LSB first
//   rx_length   bytes the master writes land in rx
//   STATUS      then read slots answer `status`, for as long as the master
//               reads (DS18B20's conversion and recall status)
//   BUSY        the command clears `status`; the model sets it again
//
// A command sets its bit in `issued`; the model takes the bits, and rx,
// from its poll with mclw_onewire_take(), and edits memory, status and
// alarm under the critical section. What the part does in a slot is
// decided at the slot before, so a change to `status` shows a slot late.

#ifndef MCLW_ONEWIRE_H
#define MCLW_ONEWIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MCLW_ONEWIRE_SEARCH_ROM 0xf0u
#define MCLW_ONEWIRE_READ_ROM 0x33u
#define MCLW_ONEWIRE_MATCH_ROM 0x55u
#define MCLW_ONEWIRE_SKIP_ROM 0xccu
#define MCLW_ONEWIRE_ALARM_SEARCH 0xecu

#define MCLW_ONEWIRE_MEMORY 16u
#define MCLW_ONEWIRE_RX_MAX 8u
#define MCLW_ONEWIRE_FUNCTIONS_MAX 16u

#define MCLW_ONEWIRE_STATUS 0x01u
#define MCLW_ONEWIRE_BUSY 0x02u

struct mclw_onewire_function {
    uint8_t command;
    uint8_t tx_offset;
    uint8_t tx_length;
    uint8_t rx_length;
    uint8_t flags;
};

struct mclw_onewire_counts {
    uint32_t resets;
    // Times the part was selected: by SKIP, a MATCH of its ROM, READ ROM,
    // or a search that ran to its last bit.
    uint32_t selected;
    uint32_t searches;
    uint32_t matches;
    uint32_t commands;
};

struct mclw_onewire {
    // The model's.
    uint8_t rom[8];
    uint8_t memory[MCLW_ONEWIRE_MEMORY];
    const struct mclw_onewire_function *functions;
    uint8_t function_count;
    volatile bool status;
    volatile bool alarm;
    // The layer's, for the model under the critical section.
    volatile uint32_t issued;
    uint8_t rx[MCLW_ONEWIRE_RX_MAX];
    volatile uint8_t rx_count;
    struct mclw_onewire_counts counts;
    // The layer's own.
    uint8_t state;
    uint8_t shift;
    uint8_t bits;
    uint8_t index;
    uint8_t function;
    uint8_t tx[MCLW_ONEWIRE_MEMORY];
    uint8_t tx_bits;
};

// Clears the layer's state and counts; the part waits for a reset.
void mclw_onewire_init(struct mclw_onewire *ow, const uint8_t rom[8],
    const struct mclw_onewire_function *functions, uint8_t function_count);

// Engine side, under the critical section.
void mclw_onewire_reset(struct mclw_onewire *ow);
// `level` is what the part sampled in the slot just ended (a write slot's
// bit, or its own 0). Returns false to hold the line low in the next slot.
bool mclw_onewire_slot(struct mclw_onewire *ow, bool level);

// Model side, under the critical section: the commands issued since the
// last take, by function index, and the bytes the last rx command got.
uint32_t mclw_onewire_take(struct mclw_onewire *ow, uint8_t *rx, uint8_t *rx_count);

// The Maxim CRC-8, X^8 + X^5 + X^4 + 1, LSB first (DS18B20 "CRC GENERATION").
uint8_t mclw_onewire_crc8(const uint8_t *data, size_t len);

#endif
