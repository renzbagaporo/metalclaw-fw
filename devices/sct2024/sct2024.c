// SPDX-License-Identifier: Apache-2.0 OR MIT
// StarChips SCT2024 16-bit serial-in, parallel-out constant-current LED
// driver: its shift register takes SDI at each rising CLK edge, and its
// latch passes the shift register to the outputs while LA/ is high and
// holds them while LA/ is low; OE/ low enables the outputs. The bits come
// from an SPI sink, framed by the controller's chip select (the part has
// none); LA/ and OE/ are inputs the DUT drives. Data sheet V01_01, Feb/08.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_sink.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/sink.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

enum {
    PROP_SHIFT,
    PROP_OUTPUTS,
    PROP_ENABLED,
    PROP_LATCHES,
};

enum {
    BINDING_SPI,
    BINDING_LA,
    BINDING_OE,
};

struct sct2024_state {
    struct mclw_spi_sink *spi;
    struct mclw_gpio *la;
    struct mclw_gpio *oe;
    struct mclw_sink sink;
    // The last 16 bits shifted in: the first in bit 15 once 16 are in.
    uint16_t shift;
    // What the latch holds: OUTk is bit k.
    uint16_t outputs;
    uint32_t latches;
};

static const struct mclw_property_desc sct2024_properties[] = {
    { .name = "shift", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 0xffff },
    // OUT15-OUT0 as the latch holds them; whether OE/ enables them.
    { .name = "outputs", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 0xffff },
    { .name = "enabled", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
    // LA/ pulses seen.
    { .name = "latches", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc sct2024_bindings[] = {
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
        .name = "la",
        .kind = MCLW_BINDING_GPIO_IN,
        .required = true,
        .usb = NULL,
    },
    {
        .name = "oe",
        .kind = MCLW_BINDING_GPIO_IN,
        .required = false,
        .usb = NULL,
    },
};

static void on_bytes(struct mclw_sink *sink, const uint16_t *entries, size_t count, bool ended, void *ctx) {
    struct sct2024_state *s = ctx;

    (void)sink;
    (void)ended;
    for (size_t i = 0; i < count; i++) {
        s->shift = (uint16_t)((s->shift << 8) | MCLW_SINK_BYTE(entries[i]));
    }
    // A transparent latch passes the bits on at once.
    if (count > 0 && mclw_hal_gpio_read(s->la)) {
        s->outputs = s->shift;
    }
}

// LA/ low holds the latch: what it held is what the shift register had
// when LA/ fell.
static void sct2024_poll(struct mclw_instance *inst) {
    struct sct2024_state *s = inst->state;

    if (mclw_hal_gpio_fell(s->la)) {
        s->outputs = s->shift;
        s->latches++;
    } else if (mclw_hal_gpio_read(s->la)) {
        s->outputs = s->shift;
    }
}

static int sct2024_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct sct2024_state *s = inst->state;

    s->spi = bindings[BINDING_SPI].spi_sink;
    s->la = bindings[BINDING_LA].gpio;
    s->oe = bindings[BINDING_OE].present ? bindings[BINDING_OE].gpio : NULL;
    s->shift = 0;
    s->outputs = 0;
    s->latches = 0;
    (void)mclw_hal_gpio_fell(s->la);
    mclw_sink_init(&s->sink, on_bytes, s);
    return mclw_hal_spi_sink_arm(s->spi, &s->sink);
}

static void sct2024_deinit(struct mclw_instance *inst) {
    struct sct2024_state *s = inst->state;

    mclw_hal_spi_sink_disarm(s->spi);
}

static int sct2024_reset(struct mclw_instance *inst, bool factory) {
    struct sct2024_state *s = inst->state;

    (void)factory;
    s->shift = 0;
    s->outputs = 0;
    s->latches = 0;
    return 0;
}

static int sct2024_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct sct2024_state *s = inst->state;

    switch (property) {
        case PROP_SHIFT:
            out->v[0] = s->shift;
            return 0;
        case PROP_OUTPUTS:
            out->v[0] = s->outputs;
            return 0;
        case PROP_ENABLED:
            out->v[0] = s->oe == NULL || !mclw_hal_gpio_read(s->oe);
            return 0;
        case PROP_LATCHES:
            out->v[0] = s->latches;
            return 0;
        default:
            return -EINVAL;
    }
}

static int sct2024_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    (void)inst;
    (void)property;
    (void)in;
    return -EINVAL;
}

static const struct mclw_model_ops sct2024_ops = {
    .init = sct2024_init,
    .deinit = sct2024_deinit,
    .reset = sct2024_reset,
    .get = sct2024_get,
    .set = sct2024_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = sct2024_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_sct2024 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "sct2024",
    .family = "sct2024",
    .config = NULL,
    .properties = sct2024_properties,
    .property_count = sizeof(sct2024_properties) / sizeof(sct2024_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = sct2024_bindings,
    .binding_count = sizeof(sct2024_bindings) / sizeof(sct2024_bindings[0]),
    .state_size = sizeof(struct sct2024_state),
    .ops = &sct2024_ops,
};
