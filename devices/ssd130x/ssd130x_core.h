// SPDX-License-Identifier: Apache-2.0 OR MIT
// The Solomon Systech SSD1306 / SSD1309 controller as a decoder of the
// command and data bytes the host sends, with its graphic display data RAM.
// No bus and no HAL: the model feeds it bytes, the host checks run it as is.
//
// Section and table numbers: SSD1306 Rev 1.1 (Apr 2008) and SSD1309 Rev 1.1
// (Jul 2011); both number their command tables 9-1 to 9-5.

#ifndef MCLW_DEVICE_SSD130X_CORE_H
#define MCLW_DEVICE_SSD130X_CORE_H

#include <stdbool.h>
#include <stdint.h>

// 128 columns by 8 pages of 8 rows; a byte is one column of one page, row
// 8p + n in bit n (8.7). The image holds page p, column c at 128p + c.
#define SSD130X_COLUMNS 128u
#define SSD130X_PAGES 8u
#define SSD130X_GDDRAM_BYTES (SSD130X_COLUMNS * SSD130X_PAGES)
#define SSD130X_PARAMS_MAX 7u

enum ssd130x_mode {
    SSD130X_MODE_HORIZONTAL = 0,
    SSD130X_MODE_VERTICAL = 1,
    SSD130X_MODE_PAGE = 2,
};

// What one part has that the other has not.
struct ssd130x_variant {
    // SSD1306 8Dh, the charge pump (application note in the datasheet,
    // section 2, table 1).
    bool charge_pump;
    // SSD1309 FDh, the command lock (10.22), DCh, the GPIO pin (10.20), and
    // 2Ch/2Dh, content scroll (10.28).
    bool command_lock;
    // Parameter bytes of 26h/27h and 29h/2Ah: 6 and 5 on the SSD1306
    // (table 9-1 part 2); 7 and 7 on the SSD1309, which adds the start and
    // end column (table 9-2).
    uint8_t horizontal_scroll_params;
    uint8_t vertical_scroll_params;
    // D5h's reset value: 80h on the SSD1306, 70h on the SSD1309 (table 9-5,
    // changed in its Rev 1.1).
    uint8_t clock_reset;
    // DBh's reset value: 20h on the SSD1306, 34h on the SSD1309.
    uint8_t vcomh_reset;
};

struct ssd130x {
    const struct ssd130x_variant *variant;
    uint8_t gddram[SSD130X_GDDRAM_BYTES];

    // A command waiting for its parameters.
    uint8_t command;
    uint8_t params[SSD130X_PARAMS_MAX];
    uint8_t want;
    uint8_t have;
    // The I2C framing (8.1.5.2): whether the next byte is a control byte,
    // and the D/C# and Co the last one gave.
    bool i2c_control;
    bool i2c_data;
    bool i2c_continue;

    enum ssd130x_mode mode;
    uint8_t column;
    uint8_t page;
    uint8_t column_start;
    uint8_t column_end;
    uint8_t page_start;
    uint8_t page_end;
    // The page addressing mode's column start (00h~0Fh, 10h~1Fh).
    uint8_t page_column;

    bool display_on;
    bool inverse;
    bool entire_on;
    bool segment_remap;
    bool com_remap;
    bool scroll;
    bool locked;
    uint8_t contrast;
    uint8_t multiplex;
    uint8_t offset;
    uint8_t start_line;
    uint8_t com_pins;
    uint8_t clock;
    uint8_t precharge;
    uint8_t vcomh;
    uint8_t charge_pump;
    uint8_t gpio;

    uint32_t data_bytes;
    uint32_t commands;
    uint32_t ignored;
};

// Power-on: the RAM is zeroed (its content is undefined) and the registers
// take their reset values.
void ssd130x_power_on(struct ssd130x *d, const struct ssd130x_variant *variant);
// RES# low (8.5 / 8.4): registers to their reset values, RAM kept.
void ssd130x_reset(struct ssd130x *d);
// A byte the host sent as a command (D/C# low) or as data (D/C# high).
void ssd130x_command_byte(struct ssd130x *d, uint8_t byte);
void ssd130x_data_byte(struct ssd130x *d, uint8_t byte);
// A byte that came over I2C, `first` after START and the address: the
// control bytes decide what the others are.
void ssd130x_i2c_byte(struct ssd130x *d, uint8_t byte, bool first);

#endif
