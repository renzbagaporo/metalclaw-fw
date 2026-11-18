// SPDX-License-Identifier: Apache-2.0 OR MIT
// Maxim MAX6675 cold-junction-compensated K-thermocouple-to-digital
// converter: a read-only SPI part whose every chip-select window clocks out
// one 16-bit frame, served as an output stream. MAX6675 data sheet
// 19-2235 Rev 1.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/spi_target.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/stream.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#define FRAME_BYTES 2u
// 0.25 C a count over 12 bits: 0 to 1023.75 C (Electrical Characteristics).
#define TEMP_MAX_QC 4095

enum {
    PROP_TEMPERATURE,
    PROP_OPEN,
    PROP_SPI_MODE,
};

enum {
    BINDING_SPI,
};

struct max6675_state {
    struct mclw_stream stream;
    struct mclw_spi_target *spi;
    int32_t temperature_mc;
    bool open;
};

static const struct mclw_property_desc max6675_properties[] = {
    { .name = "temperature", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mC",
      .min = 0, .max = 1023750 },
    { .name = "open", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = 1 },
    MCLW_PROPERTY_SPI_MODE,
};

// Serial Interface Protocol: the part shifts out on SCK's falling edge and
// the host samples on its rise, SPI mode 0 (or 3), MSB first, read-only.
static const struct mclw_binding_desc max6675_bindings[] = {
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

// Figure 2 and "Serial Interface": D15 a dummy sign bit, always 0; D14-D3
// the temperature in 0.25 C counts, MSB first; D2 high when the
// thermocouple input is open; D1 the device ID, 0; D0 three-state, read
// here as 0.
static void publish(struct mclw_instance *inst) {
    struct max6675_state *s = inst->state;
    int32_t counts = (s->temperature_mc + 125) / 250;
    uint16_t frame;

    if (counts < 0) {
        counts = 0;
    } else if (counts > TEMP_MAX_QC) {
        counts = TEMP_MAX_QC;
    }
    frame = (uint16_t)((uint16_t)counts << 3) | (s->open ? 0x0004u : 0u);
    uint8_t bytes[FRAME_BYTES] = { (uint8_t)(frame >> 8), (uint8_t)frame };
    mclw_stream_publish(&s->stream, bytes, sizeof(bytes));
    mclw_trace(inst, MCLW_TRACE_STATE, 0, FRAME_BYTES, 0);
}

static int max6675_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct max6675_state *s = inst->state;

    s->spi = bindings[BINDING_SPI].spi;
    s->temperature_mc = 25000;
    s->open = false;
    mclw_stream_init(&s->stream, MCLW_STREAM_OUT, FRAME_BYTES, 0x00);
    publish(inst);
    return mclw_hal_spi_target_arm_stream(s->spi, &s->stream);
}

static void max6675_deinit(struct mclw_instance *inst) {
    struct max6675_state *s = inst->state;

    mclw_hal_spi_target_disarm(s->spi);
    s->spi = NULL;
}

static int max6675_reset(struct mclw_instance *inst, bool factory) {
    (void)factory;
    publish(inst);
    return 0;
}

static int max6675_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct max6675_state *s = inst->state;

    switch (property) {
        case PROP_TEMPERATURE:
            out->v[0] = s->temperature_mc;
            return 0;
        case PROP_OPEN:
            out->v[0] = s->open;
            return 0;
        case PROP_SPI_MODE:
            return mclw_spi_mode_get(inst, BINDING_SPI, out);
        default:
            return -EINVAL;
    }
}

static int max6675_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct max6675_state *s = inst->state;

    switch (property) {
        case PROP_TEMPERATURE:
            s->temperature_mc = (int32_t)in->v[0];
            publish(inst);
            return 0;
        case PROP_OPEN:
            s->open = in->v[0] != 0;
            publish(inst);
            return 0;
        case PROP_SPI_MODE:
            return mclw_spi_mode_set(inst, BINDING_SPI, in);
        default:
            return -EINVAL;
    }
}

static const struct mclw_model_ops max6675_ops = {
    .init = max6675_init,
    .deinit = max6675_deinit,
    .reset = max6675_reset,
    .get = max6675_get,
    .set = max6675_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .usb_rx = NULL,
};

const struct mclw_model mclw_model_max6675 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "max6675",
    .family = "max6675",
    .config = NULL,
    .properties = max6675_properties,
    .property_count = sizeof(max6675_properties) / sizeof(max6675_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = max6675_bindings,
    .binding_count = sizeof(max6675_bindings) / sizeof(max6675_bindings[0]),
    .state_size = sizeof(struct max6675_state),
    .ops = &max6675_ops,
};
