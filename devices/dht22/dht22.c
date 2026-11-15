// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/wire.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "dht22.h"

enum {
    PROP_HUMIDITY,
    PROP_TEMPERATURE,
    PROP_RESPONSES,
};

enum {
    BINDING_DATA,
};

struct dht22_state {
    struct mclw_wire *data;
    uint32_t humidity_mpct;
    int32_t temperature_mc;
    // The waveform waits for the line to be idle when it changed mid-response.
    bool stale;
    uint32_t pulses[DHT22_PULSES];
};

static const struct mclw_property_desc dht22_properties[] = {
    {
        .name = "humidity",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "m%RH",
        .min = 0,
        .max = 100000,
    },
    {
        .name = "temperature",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mC",
        .min = DHT22_TEMPERATURE_MIN_MC,
        .max = DHT22_TEMPERATURE_MAX_MC,
    },
    {
        .name = "responses",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
};

// Section 6(2), step 1: the host pulls the bus low for at least 1 ms.
static const struct mclw_binding_desc dht22_bindings[] = {
    {
        .name = "data",
        .kind = MCLW_BINDING_WIRE,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
        .wire = { .engine = MCLW_WIRE_RESPONDER, .pins = 1, .responder = { .start_min_ns = 1000000 } },
    },
};

static uint16_t tenths(int32_t milli) {
    int32_t magnitude = milli < 0 ? -milli : milli;
    return (uint16_t)((magnitude + 50) / 100);
}

// Section 6(2): 16 bits of humidity and 16 of temperature, each in tenths,
// the temperature's top bit its sign (AM2302 "When highest bit of
// temperature is 1, it means the temperature is below 0"), then the low
// byte of the four bytes' sum; sent MSB first.
void dht22_frame(uint32_t humidity_mpct, int32_t temperature_mc, uint8_t frame[DHT22_FRAME_BYTES]) {
    uint16_t rh = tenths((int32_t)humidity_mpct);
    uint16_t t = (uint16_t)(tenths(temperature_mc) | (temperature_mc < 0 && tenths(temperature_mc) != 0u ? 0x8000u : 0u));

    frame[0] = (uint8_t)(rh >> 8);
    frame[1] = (uint8_t)rh;
    frame[2] = (uint8_t)(t >> 8);
    frame[3] = (uint8_t)t;
    frame[4] = (uint8_t)(frame[0] + frame[1] + frame[2] + frame[3]);
}

// Steps 1 to 3 and the overall diagram: after the host releases the bus
// the sensor waits 20-40 us, pulls it low 80 us, releases it 80 us, then
// sends each bit as 50 us low and 26-28 us (0) or 70 us (1) released, and
// ends with 50 us low.
void dht22_waveform(const uint8_t frame[DHT22_FRAME_BYTES], uint32_t pulses[DHT22_PULSES]) {
    size_t n = 0;

    pulses[n++] = DHT22_WAIT_NS;
    pulses[n++] = MCLW_WIRE_LOW | DHT22_RESPONSE_NS;
    pulses[n++] = DHT22_RESPONSE_NS;
    for (size_t i = 0; i < DHT22_FRAME_BYTES * 8u; i++) {
        bool one = ((frame[i / 8u] >> (7u - i % 8u)) & 1u) != 0u;
        pulses[n++] = MCLW_WIRE_LOW | DHT22_BIT_LOW_NS;
        pulses[n++] = one ? DHT22_ONE_NS : DHT22_ZERO_NS;
    }
    pulses[n++] = MCLW_WIRE_LOW | DHT22_BIT_LOW_NS;
}

static void rebuild(struct dht22_state *state) {
    uint8_t frame[DHT22_FRAME_BYTES];

    dht22_frame(state->humidity_mpct, state->temperature_mc, frame);
    dht22_waveform(frame, state->pulses);
    state->stale = mclw_hal_wire_respond(state->data, state->pulses, DHT22_PULSES) != 0;
}

static void dht22_poll(struct mclw_instance *inst) {
    struct dht22_state *state = inst->state;

    (void)mclw_hal_wire_responses(state->data);
    if (state->stale) {
        state->stale = mclw_hal_wire_respond(state->data, state->pulses, DHT22_PULSES) != 0;
    }
}

static int dht22_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct dht22_state *state = inst->state;

    state->data = bindings[BINDING_DATA].wire;
    state->humidity_mpct = DHT22_HUMIDITY_DEFAULT_MPCT;
    state->temperature_mc = DHT22_TEMPERATURE_DEFAULT_MC;
    rebuild(state);
    return state->stale ? -EIO : 0;
}

static void dht22_deinit(struct mclw_instance *inst) {
    struct dht22_state *state = inst->state;

    state->data = NULL;
}

static int dht22_reset(struct mclw_instance *inst, bool factory) {
    struct dht22_state *state = inst->state;

    if (factory) {
        state->humidity_mpct = DHT22_HUMIDITY_DEFAULT_MPCT;
        state->temperature_mc = DHT22_TEMPERATURE_DEFAULT_MC;
    }
    rebuild(state);
    return 0;
}

static int dht22_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct dht22_state *state = inst->state;

    switch (property) {
        case PROP_HUMIDITY:
            out->v[0] = state->humidity_mpct;
            return 0;
        case PROP_TEMPERATURE:
            out->v[0] = state->temperature_mc;
            return 0;
        case PROP_RESPONSES:
            out->v[0] = mclw_hal_wire_responses(state->data);
            return 0;
        default:
            return -EINVAL;
    }
}

static int dht22_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct dht22_state *state = inst->state;

    switch (property) {
        case PROP_HUMIDITY:
            state->humidity_mpct = (uint32_t)in->v[0];
            break;
        case PROP_TEMPERATURE:
            state->temperature_mc = (int32_t)in->v[0];
            break;
        default:
            return -EINVAL;
    }
    rebuild(state);
    return 0;
}

static const struct mclw_model_ops dht22_ops = {
    .init = dht22_init,
    .deinit = dht22_deinit,
    .reset = dht22_reset,
    .get = dht22_get,
    .set = dht22_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = dht22_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_dht22 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "dht22",
    .family = "dht22",
    .config = NULL,
    .properties = dht22_properties,
    .property_count = sizeof(dht22_properties) / sizeof(dht22_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = dht22_bindings,
    .binding_count = sizeof(dht22_bindings) / sizeof(dht22_bindings[0]),
    .state_size = sizeof(struct dht22_state),
    .ops = &dht22_ops,
};
