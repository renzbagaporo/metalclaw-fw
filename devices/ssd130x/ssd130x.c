// SPDX-License-Identifier: Apache-2.0 OR MIT
// The SSD1306 and SSD1309 on I2C or 4-wire SPI, each a sink: the controller
// only listens (in serial mode only writes are allowed, 8.1.3), so the
// engine hands every byte to the decoder in ssd130x_core.c.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/spi_sink.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/sink.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "ssd130x.h"
#include "ssd130x_core.h"

enum {
    PROP_DISPLAY_ON,
    PROP_ADDRESSING_MODE,
    PROP_COLUMN,
    PROP_PAGE,
    PROP_COLUMN_RANGE,
    PROP_PAGE_RANGE,
    PROP_CONTRAST,
    PROP_INVERSE,
    PROP_ENTIRE_ON,
    PROP_SEGMENT_REMAP,
    PROP_COM_REMAP,
    PROP_MULTIPLEX,
    PROP_DISPLAY_OFFSET,
    PROP_START_LINE,
    PROP_COM_PINS,
    PROP_CLOCK,
    PROP_PRECHARGE,
    PROP_VCOMH,
    PROP_SCROLL,
    PROP_DATA_BYTES,
    PROP_COMMANDS,
    // The one each part has of its own: the SSD1306's charge pump, the
    // SSD1309's command lock.
    PROP_VARIANT,
};

enum {
    BINDING_I2C,
    BINDING_SPI,
    BINDING_RESET,
};

struct ssd130x_state {
    struct ssd130x d;
    struct mclw_sink sink;
    struct mclw_i2c_target *i2c;
    struct mclw_spi_sink *spi;
    struct mclw_gpio *reset;
};

#define RO(property_name, property_type, property_count, property_min, property_max) \
    { \
        .name = (property_name), \
        .type = (property_type), \
        .count = (property_count), \
        .access = MCLW_ACCESS_RO, \
        .unit = NULL, \
        .min = (property_min), \
        .max = (property_max), \
    }

#define COMMON_PROPERTIES \
    RO("display_on", MCLW_TYPE_BOOL, 1, 0, 1), \
    RO("addressing_mode", MCLW_TYPE_U32, 1, 0, 2), \
    RO("column", MCLW_TYPE_U32, 1, 0, 127), \
    RO("page", MCLW_TYPE_U32, 1, 0, 7), \
    RO("column_range", MCLW_TYPE_U32, 2, 0, 127), \
    RO("page_range", MCLW_TYPE_U32, 2, 0, 7), \
    RO("contrast", MCLW_TYPE_U32, 1, 0, 255), \
    RO("inverse", MCLW_TYPE_BOOL, 1, 0, 1), \
    RO("entire_on", MCLW_TYPE_BOOL, 1, 0, 1), \
    RO("segment_remap", MCLW_TYPE_BOOL, 1, 0, 1), \
    RO("com_remap", MCLW_TYPE_BOOL, 1, 0, 1), \
    RO("multiplex", MCLW_TYPE_U32, 1, 16, 64), \
    RO("display_offset", MCLW_TYPE_U32, 1, 0, 63), \
    RO("start_line", MCLW_TYPE_U32, 1, 0, 63), \
    RO("com_pins", MCLW_TYPE_U32, 1, 0, 255), \
    RO("clock", MCLW_TYPE_U32, 1, 0, 255), \
    RO("precharge", MCLW_TYPE_U32, 1, 0, 255), \
    RO("vcomh", MCLW_TYPE_U32, 1, 0, 255), \
    RO("scroll", MCLW_TYPE_BOOL, 1, 0, 1), \
    RO("data_bytes", MCLW_TYPE_U32, 1, 0, UINT32_MAX), \
    RO("commands", MCLW_TYPE_U32, 1, 0, UINT32_MAX)

static const struct mclw_property_desc ssd1306_properties[] = {
    COMMON_PROPERTIES,
    RO("charge_pump", MCLW_TYPE_BOOL, 1, 0, 1),
};

static const struct mclw_property_desc ssd1309_properties[] = {
    COMMON_PROPERTIES,
    RO("locked", MCLW_TYPE_BOOL, 1, 0, 1),
};

static const struct mclw_region_desc ssd130x_regions[] = {
    { .name = "gddram", .size = SSD130X_GDDRAM_BYTES, .access = MCLW_ACCESS_RO },
};

// 4-wire SPI shifts SDIN in on every rising edge of SCLK, MSB first, and
// samples D/C# on every eighth (8.1.3, table 8-4): modes 0 and 3.
static const struct mclw_binding_desc ssd130x_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
    {
        .name = "spi",
        .kind = MCLW_BINDING_SPI_SINK,
        .required = false,
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

static const struct ssd130x_variant ssd1306_variant = {
    .charge_pump = true,
    .command_lock = false,
    .horizontal_scroll_params = 6,
    .vertical_scroll_params = 5,
    .clock_reset = 0x80,
    .vcomh_reset = 0x20,
};

static const struct ssd130x_variant ssd1309_variant = {
    .charge_pump = false,
    .command_lock = true,
    .horizontal_scroll_params = 7,
    .vertical_scroll_params = 7,
    .clock_reset = 0x70,
    .vcomh_reset = 0x34,
};

// RES# low resets the registers and holds the part while it stays low; a
// pulse shorter than the gap between two deliveries is still seen.
static bool in_reset(struct ssd130x_state *state) {
    if (state->reset == NULL) {
        return false;
    }
    bool fell = mclw_hal_gpio_fell(state->reset);
    bool low = !mclw_hal_gpio_read(state->reset);
    if (fell || low) {
        ssd130x_reset(&state->d);
    }
    return low;
}

static void on_bytes(struct mclw_sink *sink, const uint16_t *entries, size_t count, bool ended, void *ctx) {
    struct ssd130x_state *state = ctx;
    (void)sink;
    (void)ended;

    if (in_reset(state) || count == 0) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        uint8_t byte = MCLW_SINK_BYTE(entries[i]);

        if (state->i2c != NULL) {
            ssd130x_i2c_byte(&state->d, byte, (entries[i] & MCLW_SINK_FIRST) != 0);
        } else if ((entries[i] & MCLW_SINK_LINE) != 0) {
            ssd130x_data_byte(&state->d, byte);
        } else {
            ssd130x_command_byte(&state->d, byte);
        }
    }
    mclw_trace(mclw_instance_from_state(state), state->i2c != NULL ? MCLW_TRACE_I2C : MCLW_TRACE_SPI,
        MCLW_SINK_BYTE(entries[0]), (uint16_t)(count > UINT16_MAX ? UINT16_MAX : count), 0);
}

static int arm(struct ssd130x_state *state) {
    mclw_sink_init(&state->sink, on_bytes, state);
    if (state->i2c != NULL) {
        return mclw_hal_i2c_target_arm_sink(state->i2c, &state->sink);
    }
    return mclw_hal_spi_sink_arm(state->spi, &state->sink);
}

static void disarm(struct ssd130x_state *state) {
    if (state->i2c != NULL) {
        mclw_hal_i2c_target_disarm(state->i2c);
    } else if (state->spi != NULL) {
        mclw_hal_spi_sink_disarm(state->spi);
    }
}

static int ssd130x_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct ssd130x_state *state = inst->state;

    // One interface at a time, as the BS[2:0] pins select one (8.1).
    if (bindings[BINDING_I2C].present == bindings[BINDING_SPI].present) {
        return -EINVAL;
    }
    state->i2c = bindings[BINDING_I2C].present ? bindings[BINDING_I2C].i2c : NULL;
    state->spi = bindings[BINDING_SPI].present ? bindings[BINDING_SPI].spi_sink : NULL;
    state->reset = bindings[BINDING_RESET].present ? bindings[BINDING_RESET].gpio : NULL;
    ssd130x_power_on(&state->d, inst->model->config);
    if (state->reset != NULL) {
        (void)mclw_hal_gpio_fell(state->reset);
    }
    return arm(state);
}

static void ssd130x_deinit(struct mclw_instance *inst) {
    struct ssd130x_state *state = inst->state;

    disarm(state);
    state->i2c = NULL;
    state->spi = NULL;
}

static int ssd130x_reset_op(struct mclw_instance *inst, bool factory) {
    struct ssd130x_state *state = inst->state;
    (void)factory;

    disarm(state);
    ssd130x_power_on(&state->d, inst->model->config);
    return arm(state);
}

static void ssd130x_poll(struct mclw_instance *inst) {
    (void)in_reset(inst->state);
}

static int ssd130x_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct ssd130x *d = &((struct ssd130x_state *)inst->state)->d;

    switch (property) {
        case PROP_DISPLAY_ON:
            out->v[0] = d->display_on;
            return 0;
        case PROP_ADDRESSING_MODE:
            out->v[0] = d->mode;
            return 0;
        case PROP_COLUMN:
            out->v[0] = d->column;
            return 0;
        case PROP_PAGE:
            out->v[0] = d->page;
            return 0;
        case PROP_COLUMN_RANGE:
            out->v[0] = d->column_start;
            out->v[1] = d->column_end;
            return 0;
        case PROP_PAGE_RANGE:
            out->v[0] = d->page_start;
            out->v[1] = d->page_end;
            return 0;
        case PROP_CONTRAST:
            out->v[0] = d->contrast;
            return 0;
        case PROP_INVERSE:
            out->v[0] = d->inverse;
            return 0;
        case PROP_ENTIRE_ON:
            out->v[0] = d->entire_on;
            return 0;
        case PROP_SEGMENT_REMAP:
            out->v[0] = d->segment_remap;
            return 0;
        case PROP_COM_REMAP:
            out->v[0] = d->com_remap;
            return 0;
        case PROP_MULTIPLEX:
            out->v[0] = d->multiplex + 1;
            return 0;
        case PROP_DISPLAY_OFFSET:
            out->v[0] = d->offset;
            return 0;
        case PROP_START_LINE:
            out->v[0] = d->start_line;
            return 0;
        case PROP_COM_PINS:
            out->v[0] = d->com_pins;
            return 0;
        case PROP_CLOCK:
            out->v[0] = d->clock;
            return 0;
        case PROP_PRECHARGE:
            out->v[0] = d->precharge;
            return 0;
        case PROP_VCOMH:
            out->v[0] = d->vcomh;
            return 0;
        case PROP_SCROLL:
            out->v[0] = d->scroll;
            return 0;
        case PROP_DATA_BYTES:
            out->v[0] = d->data_bytes;
            return 0;
        case PROP_COMMANDS:
            out->v[0] = d->commands;
            return 0;
        case PROP_VARIANT:
            out->v[0] = d->variant->charge_pump ? (d->charge_pump & 0x04u) != 0 : d->locked;
            return 0;
        default:
            return -EINVAL;
    }
}

static int ssd130x_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf,
    size_t len) {
    const struct ssd130x_state *state = inst->state;

    if (region != 0) {
        return -EINVAL;
    }
    memcpy(buf, &state->d.gddram[offset], len);
    return 0;
}

static const struct mclw_model_ops ssd130x_ops = {
    .init = ssd130x_init,
    .deinit = ssd130x_deinit,
    .reset = ssd130x_reset_op,
    .get = ssd130x_get,
    .set = NULL,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = ssd130x_read_region,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = ssd130x_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_ssd1306 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "ssd1306",
    .family = "ssd130x",
    .config = &ssd1306_variant,
    .properties = ssd1306_properties,
    .property_count = sizeof(ssd1306_properties) / sizeof(ssd1306_properties[0]),
    .regions = ssd130x_regions,
    .region_count = 1,
    .faults = NULL,
    .fault_count = 0,
    .bindings = ssd130x_bindings,
    .binding_count = sizeof(ssd130x_bindings) / sizeof(ssd130x_bindings[0]),
    .state_size = sizeof(struct ssd130x_state),
    .ops = &ssd130x_ops,
};

const struct mclw_model mclw_model_ssd1309 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "ssd1309",
    .family = "ssd130x",
    .config = &ssd1309_variant,
    .properties = ssd1309_properties,
    .property_count = sizeof(ssd1309_properties) / sizeof(ssd1309_properties[0]),
    .regions = ssd130x_regions,
    .region_count = 1,
    .faults = NULL,
    .fault_count = 0,
    .bindings = ssd130x_bindings,
    .binding_count = sizeof(ssd130x_bindings) / sizeof(ssd130x_bindings[0]),
    .state_size = sizeof(struct ssd130x_state),
    .ops = &ssd130x_ops,
};
