// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <string.h>

#include "hd44780_core.h"

void hd44780_power_on(struct hd44780 *h) {
    memset(h, 0, sizeof(*h));
    memset(h->ddram, 0x20, sizeof(h->ddram));
    h->eight_bit = true;
    h->increment = true;
}

// The address after `ac` moving up or down (pp. 10-11, figures 2 and 4):
// the last address of a line runs on to the start of the next.
static uint8_t ddram_step(const struct hd44780 *h, uint8_t ac, bool up) {
    if (h->two_lines) {
        if (up) {
            return ac == 0x27u ? 0x40u : ac == 0x67u ? 0x00u : (uint8_t)((ac + 1u) & 0x7fu);
        }
        return ac == 0x40u ? 0x27u : ac == 0x00u ? 0x67u : (uint8_t)((ac - 1u) & 0x7fu);
    }
    if (up) {
        return ac >= 0x4fu ? 0x00u : (uint8_t)(ac + 1u);
    }
    return ac == 0x00u ? 0x4fu : (uint8_t)(ac - 1u);
}

static void move(struct hd44780 *h, bool up) {
    if (h->cgram_selected) {
        h->ac = (uint8_t)((up ? h->ac + 1u : h->ac - 1u) & 0x3fu);
    } else {
        h->ac = ddram_step(h, h->ac, up);
    }
}

static void shift_display(struct hd44780 *h, bool left) {
    uint8_t length = h->two_lines ? 40u : 80u;

    h->shift = (uint8_t)((h->shift + (left ? 1u : length - 1u)) % length);
}

// Table 6 (pp. 24-25) and the instruction descriptions (pp. 26-29).
static void instruction(struct hd44780 *h, uint8_t code) {
    h->instructions++;
    if (code & 0x80u) {
        h->ac = code & 0x7fu;
        h->cgram_selected = false;
    } else if (code & 0x40u) {
        h->ac = code & 0x3fu;
        h->cgram_selected = true;
    } else if (code & 0x20u) {
        bool eight_bit = (code & 0x10u) != 0;

        if (eight_bit != h->eight_bit) {
            h->have_high = false;
        }
        h->eight_bit = eight_bit;
        h->two_lines = (code & 0x08u) != 0;
        h->font_5x10 = (code & 0x04u) != 0;
    } else if (code & 0x10u) {
        bool right = (code & 0x04u) != 0;

        if (code & 0x08u) {
            shift_display(h, !right);
        } else {
            move(h, right);
        }
    } else if (code & 0x08u) {
        h->display_on = (code & 0x04u) != 0;
        h->cursor = (code & 0x02u) != 0;
        h->blink = (code & 0x01u) != 0;
    } else if (code & 0x04u) {
        h->increment = (code & 0x02u) != 0;
        h->shift_on_write = (code & 0x01u) != 0;
    } else if (code & 0x02u) {
        h->ac = 0;
        h->cgram_selected = false;
        h->shift = 0;
    } else if (code & 0x01u) {
        // Clear display also sets I/D to increment and leaves S (p. 26).
        memset(h->ddram, 0x20, sizeof(h->ddram));
        h->ac = 0;
        h->cgram_selected = false;
        h->shift = 0;
        h->increment = true;
    } else {
        h->instructions--;
    }
}

// With S set, a DDRAM write shifts the display the way I/D moves the
// cursor: left on increment (p. 26).
static void data(struct hd44780 *h, uint8_t value) {
    h->writes++;
    if (h->cgram_selected) {
        h->cgram[h->ac & 0x3fu] = value;
    } else {
        h->ddram[h->ac & 0x7fu] = value;
        if (h->shift_on_write) {
            shift_display(h, h->increment);
        }
    }
    move(h, h->increment);
}

void hd44780_write(struct hd44780 *h, bool rs, uint8_t db) {
    uint8_t value = db;

    if (!h->eight_bit) {
        if (!h->have_high) {
            h->high = db & 0xf0u;
            h->have_high = true;
            return;
        }
        value = (uint8_t)(h->high | (db >> 4));
        h->have_high = false;
    }
    if (rs) {
        data(h, value);
    } else {
        instruction(h, value);
    }
}
