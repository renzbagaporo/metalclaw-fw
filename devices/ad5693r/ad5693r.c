// SPDX-License-Identifier: Apache-2.0 OR MIT
// Analog Devices AD5693R 16-bit nanoDAC+ on I2C, as a sink: Table 12's
// three-byte writes (a command byte, then the data high and low bytes)
// decoded into its input, DAC and control registers. Data sheet Rev. D
// (AD5693R/AD5692R/AD5691R/AD5693).

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

// Table 12: the command in DB7-DB4.
#define CMD_NOP 0x0u
#define CMD_WRITE_INPUT 0x1u
#define CMD_UPDATE_DAC 0x2u
#define CMD_WRITE_DAC_AND_INPUT 0x3u
#define CMD_WRITE_CONTROL 0x4u

// Table 13: D15 reset, D14-D13 PD1-PD0, D12 REF (1: internal reference
// off), D11 gain (1: 0 V to 2 x VREF).
#define CONTROL_RESET 0x8000u
#define CONTROL_PD_SHIFT 13
#define CONTROL_REF 0x1000u
#define CONTROL_GAIN 0x0800u

// The internal reference.
#define VREF_INTERNAL_MV 2500u

enum {
    PROP_CODE,
    PROP_INPUT,
    PROP_CONTROL,
    PROP_VOLTAGE,
    PROP_VREF,
    PROP_WRITES,
};

enum {
    BINDING_I2C,
};

struct ad5693r_state {
    struct mclw_sink sink;
    struct mclw_i2c_target *i2c;
    uint16_t input;
    uint16_t dac;
    uint16_t control;
    uint32_t vref_mv;
    uint32_t writes;
    uint8_t frame[3];
    uint8_t index;
};

static const struct mclw_property_desc ad5693r_properties[] = {
    { .name = "code", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT16_MAX },
    { .name = "input", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT16_MAX },
    { .name = "control", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT16_MAX },
    { .name = "voltage", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = "mV",
      .min = 0, .max = 5500 },
    { .name = "vref", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mV",
      .min = 0, .max = 5500 },
    { .name = "writes", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc ad5693r_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// Figure 49: a read returns the input register, high byte first. The sink
// answers one byte for every byte read, so it holds the high byte.
static void publish(struct ad5693r_state *s) {
    s->sink.read_byte = (uint8_t)(s->input >> 8);
}

// Power-on and the software reset (Reset Bit): zero scale, and the input,
// DAC and control registers at their defaults (normal mode, internal
// reference on, gain 1).
static void defaults(struct ad5693r_state *s) {
    s->input = 0;
    s->dac = 0;
    s->control = 0;
    publish(s);
}

static uint32_t millivolts(const struct ad5693r_state *s) {
    uint32_t vref = (s->control & CONTROL_REF) != 0 ? s->vref_mv : VREF_INTERNAL_MV;
    uint32_t full = vref * ((s->control & CONTROL_GAIN) != 0 ? 2u : 1u);

    if (((s->control >> CONTROL_PD_SHIFT) & 3u) != 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)full * s->dac) / 65536u);
}

// LDAC is held high: an input write waits for the software LDAC (command
// 2), and command 3 updates both registers.
static void frame(struct ad5693r_state *s) {
    uint16_t data = (uint16_t)((s->frame[1] << 8) | s->frame[2]);

    switch (s->frame[0] >> 4) {
        case CMD_WRITE_INPUT:
            s->input = data;
            break;
        case CMD_UPDATE_DAC:
            s->dac = s->input;
            break;
        case CMD_WRITE_DAC_AND_INPUT:
            s->input = data;
            s->dac = data;
            break;
        case CMD_WRITE_CONTROL:
            if ((data & CONTROL_RESET) != 0) {
                defaults(s);
            } else {
                s->control = (uint16_t)(data & 0x7800u);
            }
            break;
        default:
            return;
    }
    s->writes++;
    publish(s);
}

// Each window carries command and data in threes (Figure 47).
static void on_bytes(struct mclw_sink *sink, const uint16_t *entries, size_t count, bool ended, void *ctx) {
    struct ad5693r_state *s = ctx;

    (void)sink;
    for (size_t i = 0; i < count; i++) {
        if ((entries[i] & MCLW_SINK_FIRST) != 0) {
            s->index = 0;
        }
        s->frame[s->index++] = MCLW_SINK_BYTE(entries[i]);
        if (s->index == sizeof(s->frame)) {
            frame(s);
            s->index = 0;
        }
    }
    if (ended) {
        s->index = 0;
    }
    if (count > 0) {
        mclw_trace(mclw_instance_from_state(s), MCLW_TRACE_I2C, MCLW_SINK_BYTE(entries[0]),
            (uint16_t)(count > UINT16_MAX ? UINT16_MAX : count), 0);
    }
}

static int ad5693r_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct ad5693r_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->vref_mv = 0;
    s->writes = 0;
    s->index = 0;
    mclw_sink_init(&s->sink, on_bytes, s);
    defaults(s);
    return mclw_hal_i2c_target_arm_sink(s->i2c, &s->sink);
}

static void ad5693r_deinit(struct mclw_instance *inst) {
    struct ad5693r_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int ad5693r_reset(struct mclw_instance *inst, bool factory) {
    struct ad5693r_state *s = inst->state;

    (void)factory;
    defaults(s);
    s->writes = 0;
    s->index = 0;
    return 0;
}

static int ad5693r_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct ad5693r_state *s = inst->state;

    switch (property) {
        case PROP_CODE:
            out->v[0] = s->dac;
            return 0;
        case PROP_INPUT:
            out->v[0] = s->input;
            return 0;
        case PROP_CONTROL:
            out->v[0] = s->control;
            return 0;
        case PROP_VOLTAGE:
            out->v[0] = millivolts(s);
            return 0;
        case PROP_VREF:
            out->v[0] = s->vref_mv;
            return 0;
        case PROP_WRITES:
            out->v[0] = s->writes;
            return 0;
        default:
            return -EINVAL;
    }
}

static int ad5693r_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct ad5693r_state *s = inst->state;

    if (property != PROP_VREF) {
        return -EINVAL;
    }
    s->vref_mv = (uint32_t)in->v[0];
    return 0;
}

static const struct mclw_model_ops ad5693r_ops = {
    .init = ad5693r_init,
    .deinit = ad5693r_deinit,
    .reset = ad5693r_reset,
    .get = ad5693r_get,
    .set = ad5693r_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_ad5693r = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "ad5693r",
    .family = "ad5693r",
    .config = NULL,
    .properties = ad5693r_properties,
    .property_count = sizeof(ad5693r_properties) / sizeof(ad5693r_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = ad5693r_bindings,
    .binding_count = sizeof(ad5693r_bindings) / sizeof(ad5693r_bindings[0]),
    .state_size = sizeof(struct ad5693r_state),
    .ops = &ad5693r_ops,
};
