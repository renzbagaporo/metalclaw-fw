// SPDX-License-Identifier: Apache-2.0 OR MIT
// The Hitachi HD44780U dot-matrix LCD controller as seen from its bus: what
// each falling edge of E does with RS, R/W and DB7..DB0. Page numbers are
// from the HD44780U datasheet, ADE-207-272(Z) '99.9 Rev 0.0.

#ifndef MCLW_DEVICE_HD44780_CORE_H
#define MCLW_DEVICE_HD44780_CORE_H

#include <stdbool.h>
#include <stdint.h>

// The DDRAM as its 7-bit address space (pp. 10-12): one line uses 00h-4Fh,
// two lines 00h-27h and 40h-67h; the rest is never addressed.
#define HD44780_DDRAM_BYTES 128u
#define HD44780_CGRAM_BYTES 64u

struct hd44780 {
    uint8_t ddram[HD44780_DDRAM_BYTES];
    uint8_t cgram[HD44780_CGRAM_BYTES];
    uint8_t ac;
    bool cgram_selected;
    bool eight_bit;
    bool two_lines;
    bool font_5x10;
    bool display_on;
    bool cursor;
    bool blink;
    bool increment;
    bool shift_on_write;
    // Display shift, in positions to the left, modulo a line's length.
    uint8_t shift;
    // In 4-bit mode, the high nibble once it has come (p. 22, figure 9).
    bool have_high;
    uint8_t high;
    uint32_t instructions;
    uint32_t writes;
};

// The internal reset circuit (p. 23): display clear, 8-bit, one line,
// 5x8 dots, display, cursor and blink off, increment, no shift.
void hd44780_power_on(struct hd44780 *h);
// One write cycle, latched at E's fall: RS and DB7..DB0 as they were while
// E was high. In 4-bit mode only DB7..DB4 are used.
void hd44780_write(struct hd44780 *h, bool rs, uint8_t db);

#endif
