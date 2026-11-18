// SPDX-License-Identifier: Apache-2.0 OR MIT
// Microchip MCP4728 quad 12-bit I2C DAC with EEPROM, as a sink: the write
// commands of Table 5-1 decoded into each channel's input register and
// EEPROM, with LDAC held low so every write reaches the outputs. Data sheet
// DS22187E.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/sink.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#define CHANNELS 4u
#define CODE_MAX 0x0fffu
// The internal reference (section 4.2), and VDD at attach.
#define VREF_INTERNAL_MV 2048u
#define VDD_MV 3300u

// Table 5-1: C2 C1 C0 in bits 7-5, W1 W0 in bits 4-3 for C 010.
enum mode {
    MODE_IDLE,
    MODE_FAST,
    MODE_MULTI,
    MODE_SEQUENTIAL,
    MODE_SINGLE,
    MODE_POWER_DOWN,
    MODE_IGNORE,
};

enum {
    PROP_CODES,
    PROP_VOLTAGES,
    PROP_VDD,
    PROP_WRITES,
};

enum {
    BINDING_I2C,
};

// One channel's configuration and data: VREF (1 internal), PD1-PD0, GX (1
// gain of 2), D11-D0, as Table 4-2 lays out the EEPROM.
struct channel {
    uint8_t vref;
    uint8_t pd;
    uint8_t gain;
    uint16_t code;
};

struct mcp4728_state {
    struct mclw_sink sink;
    struct mclw_i2c_target *i2c;
    struct channel reg[CHANNELS];
    struct channel eeprom[CHANNELS];
    uint32_t vdd_mv;
    uint32_t writes;
    enum mode mode;
    uint8_t index;
    uint8_t channel;
    uint8_t udac;
    uint8_t first;
};

static const struct mclw_property_desc mcp4728_properties[] = {
    { .name = "codes", .type = MCLW_TYPE_U32, .count = CHANNELS, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = CODE_MAX },
    { .name = "voltages", .type = MCLW_TYPE_U32, .count = CHANNELS, .access = MCLW_ACCESS_RO, .unit = "mV",
      .min = 0, .max = 5500 },
    { .name = "vdd", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mV",
      .min = 2700, .max = 5500 },
    { .name = "writes", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc mcp4728_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// Table 4-2's factory EEPROM: internal reference, normal mode, gain 1,
// code 0.
static void factory(struct mcp4728_state *s) {
    for (size_t i = 0; i < CHANNELS; i++) {
        s->eeprom[i] = (struct channel){ .vref = 1, .pd = 0, .gain = 0, .code = 0 };
    }
}

// Power-on (4.1): the input registers load from EEPROM.
static void power_on(struct mcp4728_state *s) {
    for (size_t i = 0; i < CHANNELS; i++) {
        s->reg[i] = s->eeprom[i];
    }
    s->mode = MODE_IDLE;
}

// Figure 5-15's first byte for channel A: RDY/BSY high (no EEPROM write
// pending), POR high, DAC 00, address bits 000.
#define READ_STATUS 0xc0u

static uint32_t millivolts(const struct mcp4728_state *s, size_t i) {
    const struct channel *c = &s->reg[i];
    uint32_t full;

    if (c->pd != 0) {
        return 0;
    }
    // 4.2: the internal reference with gain 1 or 2; VDD ignores gain.
    full = c->vref != 0 ? VREF_INTERNAL_MV * (c->gain != 0 ? 2u : 1u) : s->vdd_mv;
    return (uint32_t)(((uint64_t)full * c->code) / 4096u);
}

// A data pair: the upper byte's bits 3-0 are D11-D8.
static void data_byte(struct mcp4728_state *s, uint8_t b, bool with_config, bool to_eeprom) {
    struct channel *c = &s->reg[s->channel];

    if (s->index == 0) {
        if (with_config) {
            c->vref = (uint8_t)(b >> 7);
            c->gain = (uint8_t)((b >> 4) & 1u);
        }
        c->pd = (uint8_t)((b >> 5) & 3u);
        c->code = (uint16_t)((c->code & 0x00ffu) | ((uint16_t)(b & 0x0fu) << 8));
        s->index = 1;
        return;
    }
    c->code = (uint16_t)((c->code & 0x0f00u) | b);
    s->index = 0;
    s->writes++;
    if (to_eeprom) {
        s->eeprom[s->channel] = *c;
    }
}

static void command_byte(struct mcp4728_state *s, uint8_t b) {
    uint8_t c = (uint8_t)(b >> 5);

    s->index = 0;
    s->channel = (uint8_t)((b >> 1) & 3u);
    s->udac = (uint8_t)(b & 1u);
    switch (c) {
        case 0:
            // Fast Write: this byte is already channel A's upper byte.
            s->mode = MODE_FAST;
            s->channel = 0;
            data_byte(s, b, false, false);
            return;
        case 2:
            switch ((b >> 3) & 3u) {
                case 0:
                    s->mode = MODE_MULTI;
                    return;
                case 2:
                    s->mode = MODE_SEQUENTIAL;
                    return;
                case 3:
                    s->mode = MODE_SINGLE;
                    return;
                default:
                    s->mode = MODE_IGNORE;
                    return;
            }
        case 4:
            // VREF A-D in bits 3-0 (Figure 5-12).
            for (size_t i = 0; i < CHANNELS; i++) {
                s->reg[i].vref = (uint8_t)((b >> (3u - i)) & 1u);
            }
            s->writes++;
            s->mode = MODE_IGNORE;
            return;
        case 6:
            // GX A-D in bits 3-0 (Figure 5-14).
            for (size_t i = 0; i < CHANNELS; i++) {
                s->reg[i].gain = (uint8_t)((b >> (3u - i)) & 1u);
            }
            s->writes++;
            s->mode = MODE_IGNORE;
            return;
        case 5:
            // PD A and B in bits 3-0, C and D in the next byte's 7-4
            // (Figure 5-13).
            s->reg[0].pd = (uint8_t)((b >> 2) & 3u);
            s->reg[1].pd = (uint8_t)(b & 3u);
            s->mode = MODE_POWER_DOWN;
            return;
        default:
            // 011: the address bits, which take the LDAC handshake.
            s->mode = MODE_IGNORE;
            return;
    }
}

static void input(struct mcp4728_state *s, uint8_t b, bool first) {
    if (first) {
        command_byte(s, b);
        return;
    }
    switch (s->mode) {
        case MODE_FAST:
            // Pairs from A to D, then round again (Figure 5-7).
            if (s->index == 0 && s->channel == CHANNELS - 1u) {
                s->channel = 0;
            } else if (s->index == 0) {
                s->channel++;
            }
            data_byte(s, b, false, false);
            return;
        case MODE_MULTI:
            // Each channel's three bytes start with its own command byte
            // (Figure 5-8).
            if (s->first) {
                s->channel = (uint8_t)((b >> 1) & 3u);
                s->udac = (uint8_t)(b & 1u);
                s->first = 0;
                return;
            }
            data_byte(s, b, true, false);
            s->first = s->index == 0;
            return;
        case MODE_SEQUENTIAL:
            // From the selected channel to D (Figure 5-9), each to EEPROM.
            if (s->channel >= CHANNELS) {
                return;
            }
            data_byte(s, b, true, true);
            if (s->index == 0) {
                s->channel++;
            }
            return;
        case MODE_SINGLE:
            if (s->channel >= CHANNELS) {
                return;
            }
            data_byte(s, b, true, true);
            if (s->index == 0) {
                s->channel = CHANNELS;
            }
            return;
        case MODE_POWER_DOWN:
            s->reg[2].pd = (uint8_t)((b >> 6) & 3u);
            s->reg[3].pd = (uint8_t)((b >> 4) & 3u);
            s->writes++;
            s->mode = MODE_IGNORE;
            return;
        default:
            return;
    }
}

static void on_bytes(struct mclw_sink *sink, const uint16_t *entries, size_t count, bool ended, void *ctx) {
    struct mcp4728_state *s = ctx;

    (void)sink;
    for (size_t i = 0; i < count; i++) {
        bool first = (entries[i] & MCLW_SINK_FIRST) != 0;

        if (first) {
            s->first = 0;
        }
        input(s, MCLW_SINK_BYTE(entries[i]), first);
    }
    if (ended) {
        s->mode = MODE_IDLE;
    }
    if (count > 0) {
        mclw_trace(mclw_instance_from_state(s), MCLW_TRACE_I2C, MCLW_SINK_BYTE(entries[0]),
            (uint16_t)(count > UINT16_MAX ? UINT16_MAX : count), 0);
    }
}

static int mcp4728_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct mcp4728_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->vdd_mv = VDD_MV;
    s->writes = 0;
    factory(s);
    power_on(s);
    mclw_sink_init(&s->sink, on_bytes, s);
    s->sink.read_byte = READ_STATUS;
    return mclw_hal_i2c_target_arm_sink(s->i2c, &s->sink);
}

static void mcp4728_deinit(struct mclw_instance *inst) {
    struct mcp4728_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

// A power cycle reloads the input registers from EEPROM; a factory reset
// restores Table 4-2 first.
static int mcp4728_reset(struct mclw_instance *inst, bool factory_reset) {
    struct mcp4728_state *s = inst->state;

    if (factory_reset) {
        factory(s);
    }
    power_on(s);
    s->writes = 0;
    return 0;
}

static int mcp4728_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct mcp4728_state *s = inst->state;

    switch (property) {
        case PROP_CODES:
            for (size_t i = 0; i < CHANNELS; i++) {
                out->v[i] = s->reg[i].code;
            }
            return 0;
        case PROP_VOLTAGES:
            for (size_t i = 0; i < CHANNELS; i++) {
                out->v[i] = millivolts(s, i);
            }
            return 0;
        case PROP_VDD:
            out->v[0] = s->vdd_mv;
            return 0;
        case PROP_WRITES:
            out->v[0] = s->writes;
            return 0;
        default:
            return -EINVAL;
    }
}

static int mcp4728_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct mcp4728_state *s = inst->state;

    if (property != PROP_VDD) {
        return -EINVAL;
    }
    s->vdd_mv = (uint32_t)in->v[0];
    return 0;
}

static const struct mclw_model_ops mcp4728_ops = {
    .init = mcp4728_init,
    .deinit = mcp4728_deinit,
    .reset = mcp4728_reset,
    .get = mcp4728_get,
    .set = mcp4728_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_mcp4728 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "mcp4728",
    .family = "mcp4728",
    .config = NULL,
    .properties = mcp4728_properties,
    .property_count = sizeof(mcp4728_properties) / sizeof(mcp4728_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = mcp4728_bindings,
    .binding_count = sizeof(mcp4728_bindings) / sizeof(mcp4728_bindings[0]),
    .state_size = sizeof(struct mcp4728_state),
    .ops = &mcp4728_ops,
};
