// SPDX-License-Identifier: Apache-2.0 OR MIT
// The Sitronix ST7789V TFT controller on its 4-line serial interface, as a
// sink. ST7789V Version 1.3 (2014/03); section and page numbers below are
// from it.
//
// The frame memory is 240 x 320 x 18 bits (section 8.12), 172 kB, more than
// the harness has to spare, so the model keeps every eighth column of every
// eighth row of it, a 30 x 40 thumbnail, and counts what it is sent.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/spi_sink.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/sink.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "st7789v.h"

#define CMD_SWRESET 0x01u
#define CMD_SLPIN 0x10u
#define CMD_SLPOUT 0x11u
#define CMD_PTLON 0x12u
#define CMD_NORON 0x13u
#define CMD_INVOFF 0x20u
#define CMD_INVON 0x21u
#define CMD_DISPOFF 0x28u
#define CMD_DISPON 0x29u
#define CMD_CASET 0x2au
#define CMD_RASET 0x2bu
#define CMD_RAMWR 0x2cu
#define CMD_MADCTL 0x36u
#define CMD_IDMOFF 0x38u
#define CMD_IDMON 0x39u
#define CMD_COLMOD 0x3au
#define CMD_WRMEMC 0x3cu

#define COLUMNS 240u
#define ROWS 320u
#define STEP 8u

enum {
    PROP_SLEEPING,
    PROP_DISPLAY_ON,
    PROP_INVERSION,
    PROP_IDLE,
    PROP_PARTIAL,
    PROP_COLMOD,
    PROP_MADCTL,
    PROP_COLUMN_RANGE,
    PROP_ROW_RANGE,
    PROP_MEMORY_WRITES,
    PROP_PIXELS,
};

enum {
    BINDING_SPI,
    BINDING_RESET,
};

struct st7789v_state {
    uint8_t thumbnail[ST7789V_THUMBNAIL_BYTES];
    uint8_t command;
    uint8_t params[4];
    uint8_t have;
    bool sleeping;
    bool display_on;
    bool inversion;
    bool idle;
    bool partial;
    uint8_t colmod;
    uint8_t madctl;
    uint16_t column_start;
    uint16_t column_end;
    uint16_t row_start;
    uint16_t row_end;
    // The memory write pointer and the pixel being assembled.
    uint16_t column;
    uint16_t row;
    uint8_t pixel[3];
    uint8_t pixel_have;
    uint32_t memory_writes;
    uint32_t pixels;
    struct mclw_sink sink;
    struct mclw_spi_sink *spi;
    struct mclw_gpio *reset;
};

#define RO(property_name, property_count, property_max) \
    { \
        .name = (property_name), \
        .type = (property_max) == 1 ? MCLW_TYPE_BOOL : MCLW_TYPE_U32, \
        .count = (property_count), \
        .access = MCLW_ACCESS_RO, \
        .unit = NULL, \
        .min = 0, \
        .max = (property_max), \
    }

static const struct mclw_property_desc st7789v_properties[] = {
    RO("sleeping", 1, 1),
    RO("display_on", 1, 1),
    RO("inversion", 1, 1),
    RO("idle", 1, 1),
    RO("partial", 1, 1),
    RO("colmod", 1, 255),
    RO("madctl", 1, 255),
    RO("column_range", 2, 65535),
    RO("row_range", 2, 65535),
    RO("memory_writes", 1, UINT32_MAX),
    RO("pixels", 1, UINT32_MAX),
};

static const struct mclw_region_desc st7789v_regions[] = {
    { .name = "thumbnail", .size = ST7789V_THUMBNAIL_BYTES, .access = MCLW_ACCESS_RO },
};

// 4-line serial: SDA sampled on SCL's rising edge, MSB first, D/CX at the
// eighth (8.4.2, pp. 56-57): modes 0 and 3.
static const struct mclw_binding_desc st7789v_bindings[] = {
    {
        .name = "spi",
        .kind = MCLW_BINDING_SPI_SINK,
        .required = true,
        .usb = NULL,
        .spi = {
            .modes = MCLW_SPI_MODE_BIT(0) | MCLW_SPI_MODE_BIT(3),
            .default_mode = 0,
            .msb_first = true,
        },
        .uart = { 0 },
    },
    {
        .name = "reset",
        .kind = MCLW_BINDING_GPIO_IN,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// The defaults of the commands below (9.1) after power-on or a hardware
// reset. SWRESET leaves COLMOD and MADCTL (9.1.28, 9.1.32) and sets the
// window's ends by MADCTL's MV (9.1.20, 9.1.21). The frame memory is not
// cleared by either reset (9.1.22, p. 202).
static void registers_reset(struct st7789v_state *state, bool software) {
    bool mv = software && (state->madctl & 0x20u) != 0;

    state->command = 0;
    state->have = 0;
    state->sleeping = true;
    state->display_on = false;
    state->inversion = false;
    state->idle = false;
    state->partial = false;
    if (!software) {
        state->colmod = 0x66;
        state->madctl = 0;
    }
    state->column_start = 0;
    state->column_end = mv ? ROWS - 1u : COLUMNS - 1u;
    state->row_start = 0;
    state->row_end = mv ? COLUMNS - 1u : ROWS - 1u;
    state->column = 0;
    state->row = 0;
    state->pixel_have = 0;
}

static void power_on(struct st7789v_state *state) {
    memset(state->thumbnail, 0, sizeof(state->thumbnail));
    state->memory_writes = 0;
    state->pixels = 0;
    registers_reset(state, false);
}

// One pixel at the pointer, which runs along the column window, then down
// the row window, and wraps to the window's start. Addresses past the
// memory are ignored (9.1.20, p. 198).
static void store_pixel(struct st7789v_state *state, uint16_t rgb565) {
    uint16_t c = state->column;
    uint16_t r = state->row;

    state->pixels++;
    if (c < COLUMNS && r < ROWS && c % STEP == 0u && r % STEP == 0u) {
        uint32_t at = 2u * ((uint32_t)(r / STEP) * ST7789V_THUMBNAIL_COLUMNS + c / STEP);

        state->thumbnail[at] = (uint8_t)(rgb565 >> 8);
        state->thumbnail[at + 1u] = (uint8_t)rgb565;
    }
    if (state->column >= state->column_end) {
        state->column = state->column_start;
        state->row = state->row >= state->row_end ? state->row_start : (uint16_t)(state->row + 1u);
    } else {
        state->column++;
    }
}

// 8.8.42 and 8.8.43: 16 bits a pixel in two bytes, R5 G6 B5; 18 bits in
// three, six bits each in the top of a byte. 12-bit pixels are counted as
// they arrive, three bytes for two, and not stored.
static void memory_byte(struct st7789v_state *state, uint8_t byte) {
    uint8_t format = state->colmod & 0x07u;
    uint8_t size = format == 0x05u ? 2u : 3u;

    state->pixel[state->pixel_have++] = byte;
    if (state->pixel_have < size) {
        return;
    }
    state->pixel_have = 0;
    if (format == 0x05u) {
        store_pixel(state, (uint16_t)((state->pixel[0] << 8) | state->pixel[1]));
    } else if (format == 0x03u) {
        state->pixels += 2u;
    } else {
        store_pixel(state, (uint16_t)(((state->pixel[0] & 0xf8u) << 8) | ((state->pixel[1] & 0xfcu) << 3) |
            (state->pixel[2] >> 3)));
    }
}

static void command(struct st7789v_state *state, uint8_t byte) {
    state->command = byte;
    state->have = 0;
    switch (byte) {
        case CMD_SWRESET:
            registers_reset(state, true);
            break;
        case CMD_SLPIN:
            state->sleeping = true;
            break;
        case CMD_SLPOUT:
            state->sleeping = false;
            break;
        case CMD_PTLON:
            state->partial = true;
            break;
        case CMD_NORON:
            state->partial = false;
            break;
        case CMD_INVOFF:
        case CMD_INVON:
            state->inversion = byte == CMD_INVON;
            break;
        case CMD_DISPOFF:
        case CMD_DISPON:
            state->display_on = byte == CMD_DISPON;
            break;
        case CMD_IDMOFF:
        case CMD_IDMON:
            state->idle = byte == CMD_IDMON;
            break;
        // RAMWR starts at the window's start (9.1.22); WRMEMC goes on from
        // where the last write stopped (9.1.33).
        case CMD_RAMWR:
            state->column = state->column_start;
            state->row = state->row_start;
            state->pixel_have = 0;
            state->memory_writes++;
            break;
        case CMD_WRMEMC:
            state->pixel_have = 0;
            break;
        default:
            break;
    }
}

// With D/CX high a byte is the command's next parameter, or frame memory
// data after RAMWR or WRMEMC (8.4.2, p. 56).
static void parameter(struct st7789v_state *state, uint8_t byte) {
    const uint8_t *p = state->params;

    if (state->command == CMD_RAMWR || state->command == CMD_WRMEMC) {
        memory_byte(state, byte);
        return;
    }
    if (state->have < sizeof(state->params)) {
        state->params[state->have++] = byte;
    }
    switch (state->command) {
        case CMD_CASET:
            if (state->have == 4u) {
                state->column_start = (uint16_t)((p[0] << 8) | p[1]);
                state->column_end = (uint16_t)((p[2] << 8) | p[3]);
            }
            break;
        case CMD_RASET:
            if (state->have == 4u) {
                state->row_start = (uint16_t)((p[0] << 8) | p[1]);
                state->row_end = (uint16_t)((p[2] << 8) | p[3]);
            }
            break;
        case CMD_MADCTL:
            if (state->have == 1u) {
                state->madctl = byte & 0xfcu;
            }
            break;
        case CMD_COLMOD:
            if (state->have == 1u) {
                state->colmod = byte & 0x77u;
            }
            break;
        default:
            break;
    }
}

// RESX low resets (7.4.5) and holds the part while it stays low.
static bool in_reset(struct st7789v_state *state) {
    if (state->reset == NULL) {
        return false;
    }
    bool fell = mclw_hal_gpio_fell(state->reset);
    bool low = !mclw_hal_gpio_read(state->reset);
    if (fell || low) {
        registers_reset(state, false);
    }
    return low;
}

static void on_bytes(struct mclw_sink *sink, const uint16_t *entries, size_t count, bool ended, void *ctx) {
    struct st7789v_state *state = ctx;
    (void)sink;
    (void)ended;

    if (in_reset(state) || count == 0) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        uint8_t byte = MCLW_SINK_BYTE(entries[i]);

        if ((entries[i] & MCLW_SINK_LINE) != 0) {
            parameter(state, byte);
        } else {
            command(state, byte);
        }
    }
    mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_SPI, MCLW_SINK_BYTE(entries[0]),
        (uint16_t)(count > UINT16_MAX ? UINT16_MAX : count), 0);
}

static int arm(struct st7789v_state *state) {
    mclw_sink_init(&state->sink, on_bytes, state);
    return mclw_hal_spi_sink_arm(state->spi, &state->sink);
}

static int st7789v_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct st7789v_state *state = inst->state;

    state->spi = bindings[BINDING_SPI].spi_sink;
    state->reset = bindings[BINDING_RESET].present ? bindings[BINDING_RESET].gpio : NULL;
    power_on(state);
    if (state->reset != NULL) {
        (void)mclw_hal_gpio_fell(state->reset);
    }
    return arm(state);
}

static void st7789v_deinit(struct mclw_instance *inst) {
    struct st7789v_state *state = inst->state;

    mclw_hal_spi_sink_disarm(state->spi);
    state->spi = NULL;
}

static int st7789v_reset(struct mclw_instance *inst, bool factory) {
    struct st7789v_state *state = inst->state;
    (void)factory;

    mclw_hal_spi_sink_disarm(state->spi);
    power_on(state);
    return arm(state);
}

static void st7789v_poll(struct mclw_instance *inst) {
    (void)in_reset(inst->state);
}

static int st7789v_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct st7789v_state *state = inst->state;

    switch (property) {
        case PROP_SLEEPING:
            out->v[0] = state->sleeping;
            return 0;
        case PROP_DISPLAY_ON:
            out->v[0] = state->display_on;
            return 0;
        case PROP_INVERSION:
            out->v[0] = state->inversion;
            return 0;
        case PROP_IDLE:
            out->v[0] = state->idle;
            return 0;
        case PROP_PARTIAL:
            out->v[0] = state->partial;
            return 0;
        case PROP_COLMOD:
            out->v[0] = state->colmod;
            return 0;
        case PROP_MADCTL:
            out->v[0] = state->madctl;
            return 0;
        case PROP_COLUMN_RANGE:
            out->v[0] = state->column_start;
            out->v[1] = state->column_end;
            return 0;
        case PROP_ROW_RANGE:
            out->v[0] = state->row_start;
            out->v[1] = state->row_end;
            return 0;
        case PROP_MEMORY_WRITES:
            out->v[0] = state->memory_writes;
            return 0;
        case PROP_PIXELS:
            out->v[0] = state->pixels;
            return 0;
        default:
            return -EINVAL;
    }
}

static int st7789v_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf,
    size_t len) {
    const struct st7789v_state *state = inst->state;

    if (region != 0) {
        return -EINVAL;
    }
    memcpy(buf, &state->thumbnail[offset], len);
    return 0;
}

static const struct mclw_model_ops st7789v_ops = {
    .init = st7789v_init,
    .deinit = st7789v_deinit,
    .reset = st7789v_reset,
    .get = st7789v_get,
    .set = NULL,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = st7789v_read_region,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = st7789v_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_st7789v = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "st7789v",
    .family = "st7789",
    .config = NULL,
    .properties = st7789v_properties,
    .property_count = sizeof(st7789v_properties) / sizeof(st7789v_properties[0]),
    .regions = st7789v_regions,
    .region_count = 1,
    .faults = NULL,
    .fault_count = 0,
    .bindings = st7789v_bindings,
    .binding_count = sizeof(st7789v_bindings) / sizeof(st7789v_bindings[0]),
    .state_size = sizeof(struct st7789v_state),
    .ops = &st7789v_ops,
};
