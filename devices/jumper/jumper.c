// SPDX-License-Identifier: Apache-2.0 OR MIT
// A jumper wire between two DUT pins, made by the harness pins that reach
// them: `jumper` carries a push-pull signal from one pin to the other,
// `jumper_open_drain` joins two open-drain lines (an I2C bus's SDA or SCL)
// both ways. The port keeps the link up in hardware.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "jumper.h"

enum {
    PROP_LINKED,
};

struct jumper_state {
    struct mclw_gpio *pins[2];
    bool linked;
};

static const struct mclw_property_desc jumper_properties[] = {
    {
        .name = "linked",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
};

// The DUT drives `source`; the harness drives `sink` after it.
static const struct mclw_binding_desc follow_bindings[] = {
    {
        .name = "source",
        .kind = MCLW_BINDING_GPIO_IN,
        .required = true,
        .usb = NULL,
    },
    {
        .name = "sink",
        .kind = MCLW_BINDING_GPIO_OUT,
        .required = true,
        .usb = NULL,
    },
};

static const struct mclw_binding_desc join_bindings[] = {
    {
        .name = "a",
        .kind = MCLW_BINDING_GPIO_IN,
        .required = true,
        .usb = NULL,
    },
    {
        .name = "b",
        .kind = MCLW_BINDING_GPIO_IN,
        .required = true,
        .usb = NULL,
    },
};

static int follow_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct jumper_state *s = inst->state;
    int err;

    s->pins[0] = bindings[0].gpio;
    s->pins[1] = bindings[1].gpio;
    err = mclw_hal_gpio_follow(s->pins[1], s->pins[0]);
    s->linked = err == 0;
    mclw_trace(inst, MCLW_TRACE_STATE, mclw_hal_gpio_pin(s->pins[1]), 0u, (int16_t)err);
    return err;
}

static int join_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct jumper_state *s = inst->state;
    int err;

    s->pins[0] = bindings[0].gpio;
    s->pins[1] = bindings[1].gpio;
    err = mclw_hal_gpio_join(s->pins[0], s->pins[1]);
    s->linked = err == 0;
    mclw_trace(inst, MCLW_TRACE_STATE, mclw_hal_gpio_pin(s->pins[1]), 0u, (int16_t)err);
    return err;
}

static void jumper_deinit(struct mclw_instance *inst) {
    struct jumper_state *s = inst->state;

    if (s->linked) {
        mclw_hal_gpio_unlink(s->pins[0]);
        s->linked = false;
    }
}

static int jumper_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct jumper_state *s = inst->state;

    if (property != PROP_LINKED) {
        return -EINVAL;
    }
    out->v[0] = s->linked ? 1 : 0;
    return 0;
}

static const struct mclw_model_ops follow_ops = {
    .init = follow_init,
    .deinit = jumper_deinit,
    .get = jumper_get,
};

static const struct mclw_model_ops join_ops = {
    .init = join_init,
    .deinit = jumper_deinit,
    .get = jumper_get,
};

const struct mclw_model mclw_model_jumper = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "jumper",
    .family = "jumper",
    .config = NULL,
    .properties = jumper_properties,
    .property_count = sizeof(jumper_properties) / sizeof(jumper_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = follow_bindings,
    .binding_count = sizeof(follow_bindings) / sizeof(follow_bindings[0]),
    .state_size = sizeof(struct jumper_state),
    .ops = &follow_ops,
};

const struct mclw_model mclw_model_jumper_open_drain = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "jumper_open_drain",
    .family = "jumper",
    .config = NULL,
    .properties = jumper_properties,
    .property_count = sizeof(jumper_properties) / sizeof(jumper_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = join_bindings,
    .binding_count = sizeof(join_bindings) / sizeof(join_bindings[0]),
    .state_size = sizeof(struct jumper_state),
    .ops = &join_ops,
};
