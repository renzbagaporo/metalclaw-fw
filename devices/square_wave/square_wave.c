// SPDX-License-Identifier: Apache-2.0 OR MIT
// A square wave on one pin: what a signal generator, a clock, a pressed and
// released button or, behind an RC filter, a PWM level puts on a DUT pin.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "square_wave.h"

enum {
    PROP_PERIOD,
    PROP_HIGH,
    PROP_LEVEL,
    PROP_HARDWARE,
};

enum {
    BINDING_OUT,
};

struct square_wave_state {
    struct mclw_instance *inst;
    struct mclw_gpio *out;
    uint32_t period_ns;
    uint32_t high_ns;
    bool level;
    bool hardware;
    // The clock time the current period started at, for a wave on the clock.
    uint64_t start_us;
    bool driven;
};

static const struct mclw_property_desc wave_properties[] = {
    {
        .name = "period",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "ns",
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "high",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "ns",
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "level",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    {
        .name = "hardware",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
};

static const struct mclw_binding_desc wave_bindings[] = {
    {
        .name = "out",
        .kind = MCLW_BINDING_GPIO_OUT,
        .required = true,
        .usb = NULL,
    },
};

static bool level_at(const struct square_wave_state *s, uint64_t now_us) {
    if (s->period_ns == 0u) {
        return s->level;
    }
    // A tick can carry a time just before the one a property change read.
    uint64_t since = now_us > s->start_us ? now_us - s->start_us : 0u;
    return (since * 1000u) % s->period_ns < s->high_ns;
}

static void drive(struct square_wave_state *s, uint64_t now_us) {
    bool level = level_at(s, now_us);

    if (level != s->driven) {
        mclw_hal_gpio_write(s->out, level);
        s->driven = level;
    }
}

// A period too short for the clock's millisecond step goes to the port's
// hardware; every other wave follows the simulation clock.
static int apply(struct square_wave_state *s) {
    bool hardware = s->period_ns != 0u && s->period_ns < SQUARE_WAVE_CLOCK_MIN_NS;
    int err;

    if (hardware) {
        err = mclw_hal_gpio_square(s->out, s->period_ns, s->high_ns);
        if (err != 0) {
            return err;
        }
        s->hardware = true;
        mclw_trace(s->inst, MCLW_TRACE_STATE, s->period_ns, 1u, 0);
        return 0;
    }
    if (s->hardware) {
        (void)mclw_hal_gpio_square(s->out, 0u, 0u);
        s->hardware = false;
    }
    s->start_us = mclw_now_us();
    // The pin was left wherever the hardware or the last write had it.
    s->driven = !level_at(s, s->start_us);
    drive(s, s->start_us);
    mclw_trace(s->inst, MCLW_TRACE_STATE, s->period_ns, 0u, 0);
    return 0;
}

static int wave_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct square_wave_state *s = inst->state;

    s->inst = inst;
    s->out = bindings[BINDING_OUT].gpio;
    return apply(s);
}

static void wave_deinit(struct mclw_instance *inst) {
    struct square_wave_state *s = inst->state;

    if (s->hardware) {
        (void)mclw_hal_gpio_square(s->out, 0u, 0u);
        s->hardware = false;
    }
}

static int wave_reset(struct mclw_instance *inst, bool factory) {
    struct square_wave_state *s = inst->state;

    (void)factory;
    s->period_ns = 0u;
    s->high_ns = 0u;
    s->level = false;
    return apply(s);
}

static int wave_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct square_wave_state *s = inst->state;

    switch (property) {
        case PROP_PERIOD:
            out->v[0] = s->period_ns;
            return 0;
        case PROP_HIGH:
            out->v[0] = s->high_ns;
            return 0;
        case PROP_LEVEL:
            out->v[0] = s->level ? 1 : 0;
            return 0;
        case PROP_HARDWARE:
            out->v[0] = s->hardware ? 1 : 0;
            return 0;
        default:
            return -EINVAL;
    }
}

static int wave_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct square_wave_state *s = inst->state;
    struct square_wave_state before = *s;
    int err;

    switch (property) {
        case PROP_PERIOD:
            s->period_ns = (uint32_t)in->v[0];
            break;
        case PROP_HIGH:
            s->high_ns = (uint32_t)in->v[0];
            break;
        case PROP_LEVEL:
            s->level = in->v[0] != 0;
            break;
        default:
            return -EINVAL;
    }
    err = apply(s);
    if (err != 0) {
        s->period_ns = before.period_ns;
        s->high_ns = before.high_ns;
        s->level = before.level;
    }
    return err;
}

static void wave_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct square_wave_state *s = inst->state;

    if (!s->hardware) {
        drive(s, now_us);
    }
}

static const struct mclw_model_ops wave_ops = {
    .init = wave_init,
    .deinit = wave_deinit,
    .reset = wave_reset,
    .get = wave_get,
    .set = wave_set,
    .time_advanced = wave_time_advanced,
};

const struct mclw_model mclw_model_square_wave = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "square_wave",
    .family = "signal",
    .config = NULL,
    .properties = wave_properties,
    .property_count = sizeof(wave_properties) / sizeof(wave_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = wave_bindings,
    .binding_count = sizeof(wave_bindings) / sizeof(wave_bindings[0]),
    .state_size = sizeof(struct square_wave_state),
    .ops = &wave_ops,
};
