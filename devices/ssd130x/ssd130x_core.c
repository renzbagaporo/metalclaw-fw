// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <string.h>

#include "ssd130x_core.h"

// Parameter bytes after each command byte (tables 9-1 to 9-5); every other
// pattern is a single byte.
static uint8_t params_of(const struct ssd130x_variant *v, uint8_t command) {
    switch (command) {
        case 0x20:
        case 0x81:
        case 0xa8:
        case 0xd3:
        case 0xd5:
        case 0xd9:
        case 0xda:
        case 0xdb:
            return 1;
        case 0x21:
        case 0x22:
        case 0xa3:
            return 2;
        case 0x26:
        case 0x27:
            return v->horizontal_scroll_params;
        case 0x29:
        case 0x2a:
            return v->vertical_scroll_params;
        case 0x2c:
        case 0x2d:
            return v->command_lock ? 7 : 0;
        case 0x8d:
            return v->charge_pump ? 1 : 0;
        case 0xdc:
        case 0xfd:
            return v->command_lock ? 1 : 0;
        default:
            return 0;
    }
}

void ssd130x_reset(struct ssd130x *d) {
    const struct ssd130x_variant *v = d->variant;

    d->want = 0;
    d->have = 0;
    d->i2c_control = true;
    d->i2c_data = false;
    d->i2c_continue = false;
    // 8.5 (SSD1306) / 8.4 (SSD1309), and the RESET values of tables 9-1
    // to 9-5.
    d->mode = SSD130X_MODE_PAGE;
    d->column = 0;
    d->page = 0;
    d->column_start = 0;
    d->column_end = SSD130X_COLUMNS - 1u;
    d->page_start = 0;
    d->page_end = SSD130X_PAGES - 1u;
    d->page_column = 0;
    d->display_on = false;
    d->inverse = false;
    d->entire_on = false;
    d->segment_remap = false;
    d->com_remap = false;
    d->scroll = false;
    d->locked = false;
    d->contrast = 0x7f;
    d->multiplex = 63;
    d->offset = 0;
    d->start_line = 0;
    d->com_pins = 0x12;
    d->clock = v->clock_reset;
    d->precharge = 0x22;
    d->vcomh = v->vcomh_reset;
    d->charge_pump = 0x10;
    d->gpio = 0x02;
}

void ssd130x_power_on(struct ssd130x *d, const struct ssd130x_variant *variant) {
    memset(d, 0, sizeof(*d));
    d->variant = variant;
    ssd130x_reset(d);
}

static void execute(struct ssd130x *d) {
    const uint8_t *p = d->params;
    uint8_t c = d->command;

    d->commands++;
    if (c <= 0x0f) {
        d->page_column = (uint8_t)((d->page_column & 0xf0u) | c);
        if (d->mode == SSD130X_MODE_PAGE) {
            d->column = d->page_column;
        }
    } else if (c <= 0x1f) {
        d->page_column = (uint8_t)((d->page_column & 0x0fu) | ((c & 0x07u) << 4));
        if (d->mode == SSD130X_MODE_PAGE) {
            d->column = d->page_column;
        }
    } else if (c == 0x20) {
        // 11b is invalid (10.1.3) and changes nothing.
        if ((p[0] & 0x03u) != 0x03u) {
            d->mode = (enum ssd130x_mode)(p[0] & 0x03u);
        }
    } else if (c == 0x21) {
        // Also moves the column pointer to the start (10.1.4).
        d->column_start = p[0] & 0x7fu;
        d->column_end = p[1] & 0x7fu;
        d->column = d->column_start;
    } else if (c == 0x22) {
        d->page_start = p[0] & 0x07u;
        d->page_end = p[1] & 0x07u;
        d->page = d->page_start;
    } else if (c == 0x2e) {
        d->scroll = false;
    } else if (c == 0x2f) {
        d->scroll = true;
    } else if (c >= 0x40 && c <= 0x7f) {
        d->start_line = c & 0x3fu;
    } else if (c == 0x81) {
        d->contrast = p[0];
    } else if (c == 0x8d && d->variant->charge_pump) {
        d->charge_pump = p[0];
    } else if (c == 0xa0 || c == 0xa1) {
        d->segment_remap = c == 0xa1;
    } else if (c == 0xa4 || c == 0xa5) {
        d->entire_on = c == 0xa5;
    } else if (c == 0xa6 || c == 0xa7) {
        d->inverse = c == 0xa7;
    } else if (c == 0xa8) {
        // 0 to 14 are invalid entries (table 9-4).
        if ((p[0] & 0x3fu) >= 15u) {
            d->multiplex = p[0] & 0x3fu;
        }
    } else if (c == 0xae || c == 0xaf) {
        d->display_on = c == 0xaf;
    } else if (c >= 0xb0 && c <= 0xb7) {
        if (d->mode == SSD130X_MODE_PAGE) {
            d->page = c & 0x07u;
        }
    } else if (c == 0xc0 || c == 0xc8) {
        d->com_remap = c == 0xc8;
    } else if (c == 0xd3) {
        d->offset = p[0] & 0x3fu;
    } else if (c == 0xd5) {
        d->clock = p[0];
    } else if (c == 0xd9) {
        d->precharge = p[0];
    } else if (c == 0xda) {
        d->com_pins = p[0] & 0x32u;
    } else if (c == 0xdb) {
        d->vcomh = p[0];
    } else if (c == 0xdc && d->variant->command_lock) {
        d->gpio = p[0] & 0x03u;
    } else if (c == 0xfd && d->variant->command_lock) {
        d->locked = (p[0] & 0x04u) != 0;
    } else if (c == 0xe3 || c == 0x26 || c == 0x27 || c == 0x29 || c == 0x2a || c == 0xa3 ||
        ((c == 0x2c || c == 0x2d) && d->variant->command_lock)) {
        // NOP, and the scroll setups, whose effect on the panel and on the
        // RAM is not modelled (README).
    } else {
        d->commands--;
        d->ignored++;
    }
}

void ssd130x_command_byte(struct ssd130x *d, uint8_t byte) {
    if (d->want > 0) {
        d->params[d->have++] = byte;
        if (d->have == d->want) {
            d->want = 0;
            execute(d);
        }
        return;
    }
    // Locked, the part takes nothing but FDh and its parameter (10.22).
    if (d->locked && byte != 0xfd) {
        d->ignored++;
        return;
    }
    d->command = byte;
    d->have = 0;
    d->want = params_of(d->variant, byte);
    if (d->want == 0) {
        execute(d);
    }
}

// Horizontal, vertical and page addressing (10.1.3, figures 10-1, 10-3 and
// 10-4): the pointer moves after every write; in page mode the column wraps
// to the page mode's start column and the page stays.
void ssd130x_data_byte(struct ssd130x *d, uint8_t byte) {
    if (d->locked) {
        d->ignored++;
        return;
    }
    d->gddram[(uint32_t)d->page * SSD130X_COLUMNS + d->column] = byte;
    d->data_bytes++;
    switch (d->mode) {
        case SSD130X_MODE_HORIZONTAL:
            if (d->column >= d->column_end) {
                d->column = d->column_start;
                d->page = d->page >= d->page_end ? d->page_start : (uint8_t)(d->page + 1u);
            } else {
                d->column++;
            }
            break;
        case SSD130X_MODE_VERTICAL:
            if (d->page >= d->page_end) {
                d->page = d->page_start;
                d->column = d->column >= d->column_end ? d->column_start : (uint8_t)(d->column + 1u);
            } else {
                d->page++;
            }
            break;
        case SSD130X_MODE_PAGE:
        default:
            d->column = d->column >= SSD130X_COLUMNS - 1u ? d->page_column : (uint8_t)(d->column + 1u);
            break;
    }
}

// 8.1.5.2: after the address, a control byte, Co in bit 7 and D/C# in bit
// 6. With Co 0 every byte after it is of that kind until the STOP; with Co
// 1 one byte is, and the byte after it is the next control byte.
void ssd130x_i2c_byte(struct ssd130x *d, uint8_t byte, bool first) {
    if (first) {
        d->i2c_control = true;
    }
    if (d->i2c_control) {
        d->i2c_continue = (byte & 0x80u) != 0;
        d->i2c_data = (byte & 0x40u) != 0;
        d->i2c_control = false;
        return;
    }
    if (d->i2c_data) {
        ssd130x_data_byte(d, byte);
    } else {
        ssd130x_command_byte(d, byte);
    }
    if (d->i2c_continue) {
        d->i2c_control = true;
    }
}
