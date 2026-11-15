// SPDX-License-Identifier: Apache-2.0 OR MIT
// Maxim MAX7219 LED display drivers, one or a cascade of up to eight, on a
// receive-only SPI binding (DIN, CLK, LOAD). Maxim 19-4452 Rev 4 (7/03).
//
// Each part shifts DIN into a 16-bit register on CLK's rising edge and
// passes what falls out to DOUT, the next part's DIN; LOAD's rise latches
// every part's last 16 bits (Serial-Addressing Modes, p. 6). The model keeps
// the cascade's shift register and latches it at each window end. The first
// part on DIN is chip 0; after a window, it holds the last word sent.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/spi_sink.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/sink.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "max7219.h"

// Table 2, register address map.
#define REG_NOOP 0x0u
#define REG_DIGIT0 0x1u
#define REG_DIGIT7 0x8u
#define REG_DECODE 0x9u
#define REG_INTENSITY 0xau
#define REG_SCAN_LIMIT 0xbu
#define REG_SHUTDOWN 0xcu
#define REG_TEST 0xfu

enum {
    PROP_CHAIN,
    PROP_DECODE_MODE,
    PROP_INTENSITY,
    PROP_SCAN_LIMIT,
    PROP_SHUTDOWN,
    PROP_DISPLAY_TEST,
    PROP_LOADS,
};

enum {
    BINDING_SPI,
};

struct max7219_chip {
    uint8_t digits[MAX7219_DIGITS];
    uint8_t decode;
    uint8_t intensity;
    uint8_t scan_limit;
    uint8_t shutdown;
    uint8_t test;
};

struct max7219_state {
    struct max7219_chip chips[MAX7219_CHAIN_MAX];
    uint8_t chain;
    // The cascade's shift register, oldest byte first: chip k holds bytes
    // 2 (chain - 1 - k) and the one after.
    uint8_t shift[2u * MAX7219_CHAIN_MAX];
    bool open;
    uint32_t loads;
    struct mclw_sink sink;
    struct mclw_spi_sink *spi;
};

#define CHIPS(property_name, property_max) \
    { \
        .name = (property_name), \
        .type = MCLW_TYPE_U32, \
        .count = MAX7219_CHAIN_MAX, \
        .access = MCLW_ACCESS_RO, \
        .unit = NULL, \
        .min = 0, \
        .max = (property_max), \
    }

static const struct mclw_property_desc max7219_properties[] = {
    {
        .name = "chain",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 1,
        .max = MAX7219_CHAIN_MAX,
    },
    CHIPS("decode_mode", 255),
    CHIPS("intensity", 15),
    CHIPS("scan_limit", 7),
    CHIPS("shutdown", 1),
    CHIPS("display_test", 1),
    {
        .name = "loads",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
};

static const struct mclw_region_desc max7219_regions[] = {
    { .name = "digits", .size = MAX7219_CHAIN_MAX * MAX7219_DIGITS, .access = MCLW_ACCESS_RO },
};

// Data goes in on CLK's rising edge, MSB (D15) first (p. 6): modes 0 and 3.
static const struct mclw_binding_desc max7219_bindings[] = {
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
};

// Initial power-up (p. 7): every control register reset, so one digit
// scanned, no decode, minimum intensity, shutdown mode. The digit
// registers' power-up content is not given; the model starts them at 0.
static void power_on(struct max7219_state *state) {
    memset(state->chips, 0, sizeof(state->chips));
    memset(state->shift, 0, sizeof(state->shift));
    state->open = false;
    state->loads = 0;
}

// A latched word: D11-D8 the address, D7-D0 the data (table 1).
static void latch(struct max7219_chip *chip, uint8_t address, uint8_t data) {
    switch (address) {
        case REG_NOOP:
            break;
        case REG_DECODE:
            chip->decode = data;
            break;
        case REG_INTENSITY:
            chip->intensity = data & 0x0fu;
            break;
        case REG_SCAN_LIMIT:
            chip->scan_limit = data & 0x07u;
            break;
        case REG_SHUTDOWN:
            chip->shutdown = data & 0x01u;
            break;
        case REG_TEST:
            chip->test = data & 0x01u;
            break;
        default:
            if (address >= REG_DIGIT0 && address <= REG_DIGIT7) {
                chip->digits[address - REG_DIGIT0] = data;
            }
            break;
    }
}

static void load(struct max7219_state *state) {
    for (uint8_t k = 0; k < state->chain; k++) {
        const uint8_t *word = &state->shift[2u * (state->chain - 1u - k)];

        latch(&state->chips[k], word[0] & 0x0fu, word[1]);
    }
    state->loads++;
    state->open = false;
}

static void on_bytes(struct mclw_sink *sink, const uint16_t *entries, size_t count, bool ended, void *ctx) {
    struct max7219_state *state = ctx;
    size_t length = 2u * state->chain;
    (void)sink;

    for (size_t i = 0; i < count; i++) {
        if ((entries[i] & MCLW_SINK_FIRST) != 0 && state->open) {
            load(state);
        }
        memmove(state->shift, state->shift + 1, length - 1u);
        state->shift[length - 1u] = MCLW_SINK_BYTE(entries[i]);
        state->open = true;
    }
    if (ended && state->open) {
        load(state);
    }
    if (count > 0) {
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_SPI, MCLW_SINK_BYTE(entries[0]),
            (uint16_t)(count > UINT16_MAX ? UINT16_MAX : count), 0);
    }
}

static int arm(struct max7219_state *state) {
    mclw_sink_init(&state->sink, on_bytes, state);
    return mclw_hal_spi_sink_arm(state->spi, &state->sink);
}

static int max7219_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct max7219_state *state = inst->state;

    state->spi = bindings[BINDING_SPI].spi_sink;
    state->chain = 1;
    power_on(state);
    return arm(state);
}

static void max7219_deinit(struct mclw_instance *inst) {
    struct max7219_state *state = inst->state;

    mclw_hal_spi_sink_disarm(state->spi);
    state->spi = NULL;
}

static int max7219_reset(struct mclw_instance *inst, bool factory) {
    struct max7219_state *state = inst->state;
    (void)factory;

    mclw_hal_spi_sink_disarm(state->spi);
    power_on(state);
    return arm(state);
}

static int max7219_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct max7219_state *state = inst->state;

    if (property == PROP_CHAIN) {
        out->v[0] = state->chain;
        return 0;
    }
    if (property == PROP_LOADS) {
        out->v[0] = state->loads;
        return 0;
    }
    for (uint8_t k = 0; k < MAX7219_CHAIN_MAX; k++) {
        const struct max7219_chip *chip = &state->chips[k];

        switch (property) {
            case PROP_DECODE_MODE:
                out->v[k] = chip->decode;
                break;
            case PROP_INTENSITY:
                out->v[k] = chip->intensity;
                break;
            case PROP_SCAN_LIMIT:
                out->v[k] = chip->scan_limit;
                break;
            case PROP_SHUTDOWN:
                out->v[k] = chip->shutdown;
                break;
            case PROP_DISPLAY_TEST:
                out->v[k] = chip->test;
                break;
            default:
                return -EINVAL;
        }
    }
    return 0;
}

// The parts on the cascade, as wired: a new count is a new cascade, at
// power-up.
static int max7219_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct max7219_state *state = inst->state;

    if (property != PROP_CHAIN) {
        return -EINVAL;
    }
    state->chain = (uint8_t)in->v[0];
    power_on(state);
    return 0;
}

static int max7219_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf,
    size_t len) {
    const struct max7219_state *state = inst->state;
    uint8_t *out = buf;

    if (region != 0) {
        return -EINVAL;
    }
    for (size_t i = 0; i < len; i++) {
        uint32_t at = offset + (uint32_t)i;

        out[i] = state->chips[at / MAX7219_DIGITS].digits[at % MAX7219_DIGITS];
    }
    return 0;
}

static const struct mclw_model_ops max7219_ops = {
    .init = max7219_init,
    .deinit = max7219_deinit,
    .reset = max7219_reset,
    .get = max7219_get,
    .set = max7219_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = max7219_read_region,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_max7219 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "max7219",
    .family = "max7219",
    .config = NULL,
    .properties = max7219_properties,
    .property_count = sizeof(max7219_properties) / sizeof(max7219_properties[0]),
    .regions = max7219_regions,
    .region_count = 1,
    .faults = NULL,
    .fault_count = 0,
    .bindings = max7219_bindings,
    .binding_count = sizeof(max7219_bindings) / sizeof(max7219_bindings[0]),
    .state_size = sizeof(struct max7219_state),
    .ops = &max7219_ops,
};
