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
    struct mclw_stream stream;
    struct mclw_spi_target *spi;
    uint16_t sample;
};

static const struct mclw_property_desc ads7883_properties[] = {
    {
        .name = "sample",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "LSB",
        .min = 0,
        .max = ADS7883_SAMPLE_MAX,
    },
    MCLW_PROPERTY_SPI_MODE,
};

static const struct mclw_binding_desc ads7883_bindings[] = {
    {
        .name = "spi",
        .kind = MCLW_BINDING_SPI_TARGET,
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

uint16_t mclw_ads7883_frame(uint16_t sample) {
    return (uint16_t)((sample & ADS7883_SAMPLE_MAX) << 2);
}

static void publish_sample(struct mclw_instance *inst) {
    struct ads7883_state *state = inst->state;
    uint16_t frame = mclw_ads7883_frame(state->sample);
    uint8_t bytes[ADS7883_FRAME_BYTES] = { (uint8_t)(frame >> 8), (uint8_t)frame };

    mclw_stream_publish(&state->stream, bytes, sizeof(bytes));
    mclw_trace(inst, MCLW_TRACE_STATE, 0, ADS7883_FRAME_BYTES, 0);
}

static int ads7883_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct ads7883_state *state = inst->state;

    state->spi = bindings[BINDING_SPI].spi;
    state->sample = ADS7883_SAMPLE_DEFAULT;
    mclw_stream_init(&state->stream, MCLW_STREAM_OUT, ADS7883_FRAME_BYTES, 0x00);
    publish_sample(inst);
    return mclw_hal_spi_target_arm_stream(state->spi, &state->stream);
}

static void ads7883_deinit(struct mclw_instance *inst) {
    struct ads7883_state *state = inst->state;

    mclw_hal_spi_target_disarm(state->spi);
    state->spi = NULL;
}

static int ads7883_reset(struct mclw_instance *inst, bool factory) {
    struct ads7883_state *state = inst->state;

    (void)factory;
    state->sample = ADS7883_SAMPLE_DEFAULT;
    publish_sample(inst);
    return 0;
}

static int ads7883_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct ads7883_state *state = inst->state;

    switch (property) {
        case PROP_SAMPLE:
            out->v[0] = state->sample;
            return 0;
        case PROP_SPI_MODE:
            return mclw_spi_mode_get(inst, BINDING_SPI, out);
        default:
            return -EINVAL;
    }
}

static int ads7883_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct ads7883_state *state = inst->state;

    switch (property) {
        case PROP_SAMPLE:
            state->sample = (uint16_t)in->v[0];
            publish_sample(inst);
            return 0;
        case PROP_SPI_MODE:
            return mclw_spi_mode_set(inst, BINDING_SPI, in);
        default:
            return -EINVAL;
    }
}

static const struct mclw_model_ops ads7883_ops = {
    .init = ads7883_init,
    .deinit = ads7883_deinit,
    .reset = ads7883_reset,
    .get = ads7883_get,
    .set = ads7883_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .usb_rx = NULL,
};

const struct mclw_model mclw_model_ads7883 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "ads7883",
    .family = "ads7883",
    .config = NULL,
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
