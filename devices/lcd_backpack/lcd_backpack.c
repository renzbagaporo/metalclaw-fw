// SPDX-License-Identifier: Apache-2.0 OR MIT
// Adafruit's USB + Serial RGB backlight character LCD backpack (product
// 781) on its TTL serial input. The command set is the one Adafruit's
// guide documents (usb-plus-serial-backpack, "Command Reference"); where
// the guide is silent or wrong the behaviour is the backpack firmware's,
// Adafruit_USBSerialLCD.ino at adafruit/Adafruit-USB-Serial-RGB-Character-
// Backpack 9b8b221, which every line marked "firmware" follows.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/bus.h"
#include "metalclaw/hal/uart.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "lcd_backpack.h"

#define CMD_START 0xfe

#define CMD_DISPLAY_ON 0x42
#define CMD_DISPLAY_OFF 0x46
#define CMD_SET_BRIGHTNESS 0x99
#define CMD_SETSAVE_BRIGHTNESS 0x98
#define CMD_SET_CONTRAST 0x50
#define CMD_SETSAVE_CONTRAST 0x91
#define CMD_AUTOSCROLL_ON 0x51
#define CMD_AUTOSCROLL_OFF 0x52
#define CMD_CLEAR 0x58
#define CMD_SPLASH 0x40
#define CMD_SET_CURSOR 0x47
#define CMD_HOME 0x48
#define CMD_CURSOR_BACK 0x4c
#define CMD_CURSOR_FORWARD 0x4d
#define CMD_UNDERLINE_ON 0x4a
#define CMD_UNDERLINE_OFF 0x4b
#define CMD_BLOCK_ON 0x53
#define CMD_BLOCK_OFF 0x54
#define CMD_BAUD 0x39
#define CMD_CUSTOM_CHAR 0x4e
#define CMD_SAVE_CUSTOM_BANK 0xc1
#define CMD_LOAD_CUSTOM_BANK 0xc0
#define CMD_GPO_OFF 0x56
#define CMD_GPO_ON 0x57
#define CMD_GPO_START 0xc3
#define CMD_RGB 0xd0
#define CMD_SET_SIZE 0xd1
#define CMD_TEST_BAUD 0xd2

#define CUSTOM_SLOTS 8
#define CUSTOM_BYTES 8
// The firmware refuses a bank above 4, so five banks fit its EEPROM map.
#define CUSTOM_BANKS 5
#define GPO_COUNT 4
#define ARGS_MAX (LCD_BACKPACK_COLS_MAX * LCD_BACKPACK_ROWS_MAX)
#define CELLS (LCD_BACKPACK_COLS_MAX * LCD_BACKPACK_ROWS_MAX)

// The values the firmware's commented-out factory block writes: white at
// full brightness, contrast 200, 16x2, the built-in splash (an erased first
// splash byte), 9600 baud.
#define FACTORY_COLS 16
#define FACTORY_ROWS 2
#define FACTORY_CONTRAST 200
#define FACTORY_BRIGHTNESS 255
#define FACTORY_BAUD 9600

enum {
    PROP_COLUMNS,
    PROP_ROWS,
    PROP_CURSOR,
    PROP_CONTRAST,
    PROP_BRIGHTNESS,
    PROP_COLOR,
    PROP_BACKLIGHT,
    PROP_AUTOSCROLL,
    PROP_UNDERLINE_CURSOR,
    PROP_BLOCK_CURSOR,
    PROP_GPO,
    PROP_GPO_START,
    PROP_BAUD,
    PROP_COMMANDS,
    PROP_CHARACTERS,
};

enum {
    REGION_DISPLAY,
    REGION_SPLASH,
    REGION_CUSTOM,
    REGION_BANKS,
};

enum {
    BINDING_UART,
};

// What the firmware keeps in EEPROM, so a power cycle keeps it.
struct saved {
    uint8_t cols;
    uint8_t rows;
    uint8_t contrast;
    uint8_t brightness;
    uint8_t color[3];
    bool autoscroll;
    uint32_t baud;
    uint8_t gpo_start;
    uint8_t splash[CELLS];
    uint8_t banks[CUSTOM_BANKS][CUSTOM_SLOTS][CUSTOM_BYTES];
};

struct lcd_backpack_state {
    struct mclw_uart *uart;
    struct saved saved;
    uint8_t display[CELLS];
    uint8_t custom[CUSTOM_SLOTS][CUSTOM_BYTES];
    uint8_t x;
    uint8_t y;
    bool backlight;
    bool underline;
    bool block;
    uint8_t gpo;
    uint32_t commands;
    uint32_t characters;
    // A command in progress: 0xFE seen, then the command byte, then its
    // arguments as they arrive.
    bool in_command;
    bool have_command;
    uint8_t command;
    uint8_t want;
    uint8_t got;
    uint8_t args[ARGS_MAX];
};

static const struct mclw_property_desc lcd_backpack_properties[] = {
    { .name = "columns", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "rows", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "cursor", .type = MCLW_TYPE_U32, .count = 2, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 1, .max = 256 },
    { .name = "contrast", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "brightness", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "color", .type = MCLW_TYPE_U32, .count = 3, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 255 },
    { .name = "backlight", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "autoscroll", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "underline_cursor", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "block_cursor", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    { .name = "gpo", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 15 },
    { .name = "gpo_start", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 15 },
    { .name = "baud", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "baud",
      .min = 1200, .max = 115200 },
    { .name = "commands", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "characters", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_region_desc lcd_backpack_regions[] = {
    { .name = "display", .size = CELLS, .access = MCLW_ACCESS_RO },
    { .name = "splash", .size = CELLS, .access = MCLW_ACCESS_RO },
    { .name = "custom", .size = CUSTOM_SLOTS * CUSTOM_BYTES, .access = MCLW_ACCESS_RO },
    { .name = "banks", .size = CUSTOM_BANKS * CUSTOM_SLOTS * CUSTOM_BYTES, .access = MCLW_ACCESS_RO },
};

// Guide, "Testing with TTL Serial": 9600 baud, 8 bits, no parity, 1 stop bit.
static const struct mclw_binding_desc lcd_backpack_bindings[] = {
    {
        .name = "uart",
        .kind = MCLW_BINDING_UART,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = {
            .default_baud = FACTORY_BAUD,
            .format = { .data_bits = 8, .stop_bits = 1, .parity = MCLW_UART_PARITY_NONE },
        },
    },
};

static uint8_t *cell(struct lcd_backpack_state *s, uint8_t x, uint8_t y) {
    return &s->display[(size_t)y * LCD_BACKPACK_COLS_MAX + x];
}

static void clear_display(struct lcd_backpack_state *s) {
    memset(s->display, ' ', sizeof(s->display));
    s->x = 0;
    s->y = 0;
}

static void factory(struct saved *saved) {
    saved->cols = FACTORY_COLS;
    saved->rows = FACTORY_ROWS;
    saved->contrast = FACTORY_CONTRAST;
    saved->brightness = FACTORY_BRIGHTNESS;
    memset(saved->color, 0xff, sizeof(saved->color));
    saved->autoscroll = false;
    saved->baud = FACTORY_BAUD;
    saved->gpo_start = 0;
    memset(saved->splash, 0xff, sizeof(saved->splash));
    memset(saved->banks, 0xff, sizeof(saved->banks));
}

// Firmware setup(): the splash shows for a second and is cleared, the
// backlight comes on at the saved brightness, GPOs take their start states.
static void power_up(struct lcd_backpack_state *s) {
    clear_display(s);
    memset(s->custom, 0, sizeof(s->custom));
    s->backlight = true;
    s->underline = false;
    s->block = false;
    s->gpo = s->saved.gpo_start;
    s->in_command = false;
    s->have_command = false;
    if (s->uart != NULL) {
        (void)mclw_hal_uart_set_baud(s->uart, s->saved.baud);
    }
}

// Firmware loop(): after a character the cursor moves on; past the last
// column it goes to the next row, and past the last row to the top left,
// or with autoscroll every row moves up and the bottom one is blanked.
static void advance(struct lcd_backpack_state *s) {
    s->x++;
    if (s->x < s->saved.cols) {
        return;
    }
    s->x = 0;
    s->y++;
    if (s->y < s->saved.rows) {
        return;
    }
    if (!s->saved.autoscroll) {
        s->y = 0;
        return;
    }
    for (uint8_t row = 0; row + 1u < s->saved.rows; row++) {
        memcpy(cell(s, 0, row), cell(s, 0, (uint8_t)(row + 1u)), s->saved.cols);
    }
    memset(cell(s, 0, (uint8_t)(s->saved.rows - 1u)), ' ', s->saved.cols);
    s->y = (uint8_t)(s->saved.rows - 1u);
}

static void cursor_back(struct lcd_backpack_state *s) {
    if (s->x > 0) {
        s->x--;
    } else {
        s->x = (uint8_t)(s->saved.cols - 1u);
        s->y = s->y > 0 ? (uint8_t)(s->y - 1u) : (uint8_t)(s->saved.rows - 1u);
    }
}

// Firmware: CR fills the rest of the row with spaces and moves to the next
// row, LF is ignored (the guide has the two the other way round), backspace
// blanks the previous cell and leaves the cursor there.
static void text(struct lcd_backpack_state *s, uint8_t c) {
    s->characters++;
    if (c == '\n') {
        return;
    }
    if (c == '\b') {
        cursor_back(s);
        *cell(s, s->x, s->y) = ' ';
        return;
    }
    if (c == '\r') {
        while (s->x < s->saved.cols) {
            *cell(s, s->x, s->y) = ' ';
            s->x++;
        }
        s->x = (uint8_t)(s->saved.cols - 1u);
    } else {
        *cell(s, s->x, s->y) = c;
    }
    advance(s);
}

static uint8_t arg_count(const struct lcd_backpack_state *s, uint8_t command) {
    switch (command) {
        case CMD_DISPLAY_ON:
        case CMD_SET_BRIGHTNESS:
        case CMD_SETSAVE_BRIGHTNESS:
        case CMD_SET_CONTRAST:
        case CMD_SETSAVE_CONTRAST:
        case CMD_BAUD:
        case CMD_LOAD_CUSTOM_BANK:
        case CMD_GPO_OFF:
        case CMD_GPO_ON:
            return 1;
        case CMD_SET_CURSOR:
        case CMD_GPO_START:
        case CMD_SET_SIZE:
            return 2;
        case CMD_RGB:
            return 3;
        case CMD_CUSTOM_CHAR:
            return 1 + CUSTOM_BYTES;
        case CMD_SAVE_CUSTOM_BANK:
            return 2 + CUSTOM_BYTES;
        case CMD_SPLASH:
            return (uint8_t)(s->saved.cols * s->saved.rows);
        default:
            return 0;
    }
}

static void send_baud(struct lcd_backpack_state *s) {
    char digits[10];
    size_t n = 0;
    uint32_t v = s->saved.baud;

    do {
        digits[sizeof(digits) - 1u - n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0 && n < sizeof(digits));
    (void)mclw_hal_uart_send(s->uart, (const uint8_t *)&digits[sizeof(digits) - n], n);
}

static void set_baud_code(struct lcd_backpack_state *s, uint8_t code) {
    static const struct {
        uint8_t code;
        uint32_t baud;
    } rates[] = {
        { 0x53, 1200 }, { 0x29, 2400 }, { 0xcf, 4800 }, { 0x67, 9600 }, { 0x33, 19200 },
        { 0x22, 28800 }, { 0x19, 38400 }, { 0x10, 57600 }, { 0x08, 115200 },
    };

    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (rates[i].code == code) {
            s->saved.baud = rates[i].baud;
            (void)mclw_hal_uart_set_baud(s->uart, s->saved.baud);
        }
    }
}

static void execute(struct lcd_backpack_state *s) {
    const uint8_t *a = s->args;

    s->commands++;
    switch (s->command) {
        case CMD_DISPLAY_ON:
            s->backlight = true;
            break;
        case CMD_DISPLAY_OFF:
            s->backlight = false;
            break;
        case CMD_SET_BRIGHTNESS:
        case CMD_SETSAVE_BRIGHTNESS:
            s->saved.brightness = a[0];
            s->backlight = true;
            break;
        case CMD_SET_CONTRAST:
        case CMD_SETSAVE_CONTRAST:
            s->saved.contrast = a[0];
            break;
        case CMD_HOME:
            s->x = 0;
            s->y = 0;
            break;
        case CMD_CLEAR:
            clear_display(s);
            break;
        case CMD_AUTOSCROLL_ON:
            s->saved.autoscroll = true;
            break;
        case CMD_AUTOSCROLL_OFF:
            s->saved.autoscroll = false;
            break;
        case CMD_SET_CURSOR: {
            // Firmware: one-based, wrapping a column past the last into the
            // rows below and a row past the last to the top.
            uint8_t x = (uint8_t)(a[0] - 1u);
            uint8_t y = (uint8_t)(a[1] - 1u);
            if (x >= s->saved.cols) {
                y = (uint8_t)(y + x / s->saved.cols);
                x = (uint8_t)(x % s->saved.cols);
            }
            s->x = x;
            s->y = (uint8_t)(y % s->saved.rows);
            break;
        }
        case CMD_CURSOR_BACK:
            cursor_back(s);
            break;
        case CMD_CURSOR_FORWARD:
            if (s->x + 1u < s->saved.cols) {
                s->x++;
            } else {
                s->x = 0;
                s->y = s->y + 1u < s->saved.rows ? (uint8_t)(s->y + 1u) : 0;
            }
            break;
        case CMD_UNDERLINE_ON:
            s->underline = true;
            break;
        case CMD_BLOCK_ON:
            s->block = true;
            break;
        // Firmware: either "off" turns both cursors off.
        case CMD_UNDERLINE_OFF:
        case CMD_BLOCK_OFF:
            s->underline = false;
            s->block = false;
            break;
        case CMD_SPLASH:
            for (uint8_t row = 0; row < s->saved.rows; row++) {
                memcpy(&s->saved.splash[(size_t)row * LCD_BACKPACK_COLS_MAX], &a[row * s->saved.cols],
                    s->saved.cols);
            }
            break;
        case CMD_BAUD:
            set_baud_code(s, a[0]);
            break;
        case CMD_CUSTOM_CHAR:
            memcpy(s->custom[a[0] % CUSTOM_SLOTS], &a[1], CUSTOM_BYTES);
            break;
        case CMD_SAVE_CUSTOM_BANK:
            if (a[0] < CUSTOM_BANKS) {
                memcpy(s->saved.banks[a[0]][a[1] % CUSTOM_SLOTS], &a[2], CUSTOM_BYTES);
            }
            break;
        case CMD_LOAD_CUSTOM_BANK:
            if (a[0] < CUSTOM_BANKS) {
                memcpy(s->custom, s->saved.banks[a[0]], sizeof(s->custom));
            }
            break;
        case CMD_GPO_OFF:
        case CMD_GPO_ON:
            if (a[0] >= 1 && a[0] <= GPO_COUNT) {
                uint8_t bit = (uint8_t)(1u << (a[0] - 1u));
                s->gpo = s->command == CMD_GPO_ON ? (uint8_t)(s->gpo | bit) : (uint8_t)(s->gpo & ~bit);
            }
            break;
        case CMD_GPO_START:
            if (a[0] >= 1 && a[0] <= GPO_COUNT) {
                uint8_t bit = (uint8_t)(1u << (a[0] - 1u));
                s->saved.gpo_start = a[1] != 0 ? (uint8_t)(s->saved.gpo_start | bit)
                                                : (uint8_t)(s->saved.gpo_start & ~bit);
            }
            break;
        case CMD_RGB:
            memcpy(s->saved.color, a, 3);
            s->backlight = true;
            break;
        // The firmware takes any size and indexes a 20x4 buffer with it; the
        // model holds the size to what that buffer can show.
        case CMD_SET_SIZE:
            s->saved.cols = a[0] == 0 ? 1 : (a[0] > LCD_BACKPACK_COLS_MAX ? LCD_BACKPACK_COLS_MAX : a[0]);
            s->saved.rows = a[1] == 0 ? 1 : (a[1] > LCD_BACKPACK_ROWS_MAX ? LCD_BACKPACK_ROWS_MAX : a[1]);
            s->x = (uint8_t)(s->x % s->saved.cols);
            s->y = (uint8_t)(s->y % s->saved.rows);
            break;
        case CMD_TEST_BAUD:
            send_baud(s);
            break;
        default:
            break;
    }
    mclw_trace(mclw_instance_from_state(s), MCLW_TRACE_COMMAND, s->command, s->got, 0);
}

static void rx_byte(struct lcd_backpack_state *s, uint8_t c) {
    if (!s->in_command) {
        if (c == CMD_START) {
            s->in_command = true;
            s->have_command = false;
        } else {
            text(s, c);
        }
        return;
    }
    if (!s->have_command) {
        s->command = c;
        s->have_command = true;
        s->want = arg_count(s, c);
        s->got = 0;
    } else {
        s->args[s->got++] = c;
    }
    if (s->got >= s->want) {
        s->in_command = false;
        execute(s);
    }
}

static void on_rx(struct mclw_uart *uart, const uint8_t *bytes, size_t len, void *ctx) {
    struct lcd_backpack_state *s = ctx;

    (void)uart;
    for (size_t i = 0; i < len; i++) {
        rx_byte(s, bytes[i]);
    }
}

static int lcd_backpack_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct lcd_backpack_state *s = inst->state;

    s->uart = bindings[BINDING_UART].uart;
    factory(&s->saved);
    power_up(s);
    s->commands = 0;
    s->characters = 0;
    mclw_hal_uart_set_rx(s->uart, on_rx, s);
    return 0;
}

static void lcd_backpack_deinit(struct mclw_instance *inst) {
    struct lcd_backpack_state *s = inst->state;

    mclw_hal_uart_set_rx(s->uart, NULL, NULL);
    s->uart = NULL;
}

// A power cycle keeps what the firmware saves in EEPROM; factory restores
// what its factory block writes.
static int lcd_backpack_reset(struct mclw_instance *inst, bool factory_reset) {
    struct lcd_backpack_state *s = inst->state;

    if (factory_reset) {
        factory(&s->saved);
    }
    power_up(s);
    s->commands = 0;
    s->characters = 0;
    return 0;
}

static int lcd_backpack_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct lcd_backpack_state *s = inst->state;

    switch (property) {
        case PROP_COLUMNS:
            out->v[0] = s->saved.cols;
            return 0;
        case PROP_ROWS:
            out->v[0] = s->saved.rows;
            return 0;
        case PROP_CURSOR:
            out->v[0] = s->x + 1;
            out->v[1] = s->y + 1;
            return 0;
        case PROP_CONTRAST:
            out->v[0] = s->saved.contrast;
            return 0;
        case PROP_BRIGHTNESS:
            out->v[0] = s->saved.brightness;
            return 0;
        case PROP_COLOR:
            for (size_t i = 0; i < 3; i++) {
                out->v[i] = s->saved.color[i];
            }
            return 0;
        case PROP_BACKLIGHT:
            out->v[0] = s->backlight;
            return 0;
        case PROP_AUTOSCROLL:
            out->v[0] = s->saved.autoscroll;
            return 0;
        case PROP_UNDERLINE_CURSOR:
            out->v[0] = s->underline;
            return 0;
        case PROP_BLOCK_CURSOR:
            out->v[0] = s->block;
            return 0;
        case PROP_GPO:
            out->v[0] = s->gpo;
            return 0;
        case PROP_GPO_START:
            out->v[0] = s->saved.gpo_start;
            return 0;
        case PROP_BAUD:
            out->v[0] = s->saved.baud;
            return 0;
        case PROP_COMMANDS:
            out->v[0] = s->commands;
            return 0;
        case PROP_CHARACTERS:
            out->v[0] = s->characters;
            return 0;
        default:
            return -EINVAL;
    }
}

// The baud rate a previous owner saved: only the rates the command offers.
static int lcd_backpack_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct lcd_backpack_state *s = inst->state;
    static const uint32_t rates[] = { 1200, 2400, 4800, 9600, 19200, 28800, 38400, 57600, 115200 };

    if (property != PROP_BAUD) {
        return -EINVAL;
    }
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (rates[i] == (uint32_t)in->v[0]) {
            s->saved.baud = rates[i];
            return mclw_hal_uart_set_baud(s->uart, s->saved.baud);
        }
    }
    return -EINVAL;
}

static int lcd_backpack_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf,
    size_t len) {
    struct lcd_backpack_state *s = inst->state;
    const uint8_t *src;

    switch (region) {
        case REGION_DISPLAY:
            src = s->display;
            break;
        case REGION_SPLASH:
            src = s->saved.splash;
            break;
        case REGION_CUSTOM:
            src = &s->custom[0][0];
            break;
        case REGION_BANKS:
            src = &s->saved.banks[0][0][0];
            break;
        default:
            return -EINVAL;
    }
    memcpy(buf, src + offset, len);
    return 0;
}

static const struct mclw_model_ops lcd_backpack_ops = {
    .init = lcd_backpack_init,
    .deinit = lcd_backpack_deinit,
    .reset = lcd_backpack_reset,
    .get = lcd_backpack_get,
    .set = lcd_backpack_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = lcd_backpack_read_region,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_lcd_backpack = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "lcd_backpack",
    .family = "lcd_backpack",
    .config = NULL,
    .properties = lcd_backpack_properties,
    .property_count = sizeof(lcd_backpack_properties) / sizeof(lcd_backpack_properties[0]),
    .regions = lcd_backpack_regions,
    .region_count = sizeof(lcd_backpack_regions) / sizeof(lcd_backpack_regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = lcd_backpack_bindings,
    .binding_count = sizeof(lcd_backpack_bindings) / sizeof(lcd_backpack_bindings[0]),
    .state_size = sizeof(struct lcd_backpack_state),
    .ops = &lcd_backpack_ops,
};
