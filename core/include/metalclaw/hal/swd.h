// SPDX-License-Identifier: Apache-2.0 OR MIT
// The SWD wire, as the CMSIS-DAP engine sees it. A port implements these
// over PIO, bit-banged GPIO, or anything else that can clock a bit.
//
// There is deliberately no write_abort here: DAP_WriteABORT is a DP write
// to address 0 and nothing else, so a correct transfer() already covers it.
// The old implementation gave each backend its own entry point and both
// backends got it wrong separately.

#ifndef MCLW_HAL_SWD_H
#define MCLW_HAL_SWD_H

#include <stdbool.h>
#include <stdint.h>

struct mclw_swd;

// DAP_SWJ_Pins bit positions, shared with the CMSIS-DAP protocol.
#define MCLW_SWD_PIN_SWCLK  (1u << 0)
#define MCLW_SWD_PIN_SWDIO  (1u << 1)
#define MCLW_SWD_PIN_NRESET (1u << 7)

// Transfer responses, CMSIS-DAP encoding: ACK in bits 0-2, bit 3 protocol
// error, bit 4 value mismatch.
#define MCLW_SWD_ACK_OK    1u
#define MCLW_SWD_ACK_WAIT  2u
#define MCLW_SWD_ACK_FAULT 4u
#define MCLW_SWD_ERROR     8u

// Claims swclk, swdio and, if not MCLW_PIN_NONE, nreset for `owner`.
// The wire stays idle until port_on. NULL with *err set when refused.
struct mclw_swd *mclw_hal_swd_open(uint8_t swclk, uint8_t swdio, uint8_t nreset,
    const void *owner, int *err);
void mclw_hal_swd_close(struct mclw_swd *swd);

int mclw_hal_swd_port_on(struct mclw_swd *swd);
int mclw_hal_swd_port_off(struct mclw_swd *swd);
int mclw_hal_swd_set_clock(struct mclw_swd *swd, uint32_t hz);
uint32_t mclw_hal_swd_get_clock(const struct mclw_swd *swd);
int mclw_hal_swd_configure(struct mclw_swd *swd, uint8_t turnaround, bool data_phase);

// `count` bits, LSB first, packed 8 per byte.
int mclw_hal_swd_output_sequence(struct mclw_swd *swd, uint32_t count, const uint8_t *data);
int mclw_hal_swd_input_sequence(struct mclw_swd *swd, uint32_t count, uint8_t *data);

// One DP/AP transfer. `request` is the CMSIS-DAP request byte (APnDP, RnW,
// A2, A3). On a read the word lands in *data; on a write it is taken from
// *data. Returns 0 and sets *response, or a negative errno when the wire
// itself failed.
int mclw_hal_swd_transfer(struct mclw_swd *swd, uint8_t request, uint32_t *data,
    uint8_t idle_cycles, uint8_t *response);

// Direct pin control for DAP_SWJ_Pins. Only pins in `mask` change.
int mclw_hal_swd_set_pins(struct mclw_swd *swd, uint8_t mask, uint8_t values);
int mclw_hal_swd_get_pins(struct mclw_swd *swd, uint8_t *values);

#endif
