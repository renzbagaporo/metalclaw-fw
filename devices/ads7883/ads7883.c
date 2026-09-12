// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/stream.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "ads7883.h"

enum {
    PROP_SAMPLE,
    PROP_SPI_MODE,
};

enum {
    BINDING_SPI,
};

struct ads7883_state {
    struct metalclaw_stream stream;
    struct metalclaw_spi_target *spi;
    uint16_t sample;
    uint8_t spi_mode;
};

static const struct metalclaw_property_desc ads7883_properties[] = {
    {
        .name = "sample",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RW,
        .unit = "LSB",
        .min = 0,
        .max = ADS7883_SAMPLE_MAX,
    },
    {
        .name = "spi_mode",
        .type = METALCLAW_TYPE_U32,
        .count = 1,
        .access = METALCLAW_ACCESS_RW,
        .unit = NULL,
        .min = 0,
        .max = 3,
    },
};

static const struct metalclaw_binding_desc ads7883_bindings[] = {
    {
        .name = "spi",
        .kind = METALCLAW_BINDING_SPI_TARGET,
        .required = true,
        .usb = NULL,
        .spi = {
            .modes = METALCLAW_SPI_MODE_BIT(0) | METALCLAW_SPI_MODE_BIT(3),
            .default_mode = 0,
            .msb_first = true,
        },
        .uart = { 0 },
    },
};

uint16_t metalclaw_ads7883_frame(uint16_t sample) {
    return (uint16_t)((sample & ADS7883_SAMPLE_MAX) << 2);
}

static void publish_sample(struct metalclaw_instance *inst) {
    struct ads7883_state *state = inst->state;
    uint16_t frame = metalclaw_ads7883_frame(state->sample);
    uint8_t bytes[ADS7883_FRAME_BYTES] = { (uint8_t)(frame >> 8), (uint8_t)frame };

    metalclaw_stream_publish(&state->stream, bytes, sizeof(bytes));
    metalclaw_trace(inst, METALCLAW_TRACE_STATE, 0, ADS7883_FRAME_BYTES, 0);
}

static int ads7883_init(struct metalclaw_instance *inst, const struct metalclaw_binding *bindings) {
    struct ads7883_state *state = inst->state;

    if (!bindings[BINDING_SPI].present) {
        return -EINVAL;
    }
    state->spi = bindings[BINDING_SPI].spi;
    state->sample = ADS7883_SAMPLE_DEFAULT;
    state->spi_mode = ads7883_bindings[BINDING_SPI].spi.default_mode;
    metalclaw_stream_init(&state->stream, METALCLAW_STREAM_OUT, ADS7883_FRAME_BYTES, 0x00);
    publish_sample(inst);
    return metalclaw_hal_spi_target_arm_stream(state->spi, &state->stream);
}

static void ads7883_deinit(struct metalclaw_instance *inst) {
    struct ads7883_state *state = inst->state;

    metalclaw_hal_spi_target_disarm(state->spi);
    state->spi = NULL;
}

static int ads7883_reset(struct metalclaw_instance *inst, bool factory) {
    struct ads7883_state *state = inst->state;

    (void)factory;
    state->sample = ADS7883_SAMPLE_DEFAULT;
    publish_sample(inst);
    return 0;
}

static int ads7883_get(struct metalclaw_instance *inst, size_t property, struct metalclaw_value *out) {
    struct ads7883_state *state = inst->state;

    switch (property) {
        case PROP_SAMPLE:
            out->v[0] = state->sample;
            return 0;
        case PROP_SPI_MODE:
            out->v[0] = state->spi_mode;
            return 0;
        default:
            return -EINVAL;
    }
}

static int ads7883_set(struct metalclaw_instance *inst, size_t property, const struct metalclaw_value *in) {
    struct ads7883_state *state = inst->state;

    switch (property) {
        case PROP_SAMPLE:
            state->sample = (uint16_t)in->v[0];
            publish_sample(inst);
            return 0;
        case PROP_SPI_MODE: {
            uint8_t mode = (uint8_t)in->v[0];
            if ((ads7883_bindings[BINDING_SPI].spi.modes & METALCLAW_SPI_MODE_BIT(mode)) == 0) {
                return -EINVAL;
            }
            int err = metalclaw_hal_spi_target_set_mode(state->spi, mode);
            if (err == 0) {
                state->spi_mode = mode;
            }
            return err;
        }
        default:
            return -EINVAL;
    }
}

static const struct metalclaw_model_ops ads7883_ops = {
    .init = ads7883_init,
    .deinit = ads7883_deinit,
    .reset = ads7883_reset,
    .get = ads7883_get,
    .set = ads7883_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .prepare = NULL,
    .time_advanced = NULL,
    .usb_rx = NULL,
};

const struct metalclaw_model metalclaw_model_ads7883 = {
    .abi_version = METALCLAW_ABI_VERSION,
    .id = "ads7883",
    .family = "ads7883",
    .properties = ads7883_properties,
    .property_count = sizeof(ads7883_properties) / sizeof(ads7883_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = ads7883_bindings,
    .binding_count = sizeof(ads7883_bindings) / sizeof(ads7883_bindings[0]),
    .state_size = sizeof(struct ads7883_state),
    .ops = &ads7883_ops,
};
