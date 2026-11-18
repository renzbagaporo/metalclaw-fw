// SPDX-License-Identifier: Apache-2.0 OR MIT
// TI DRV2605L haptic driver on I2C: Table 3's registers 00h-22h as a
// register map the harness serves from hardware, with the waveform
// sequencer played on GO and the self-clearing DEV_RESET and GO bits.
// Data sheet SLOS854D (March 2018).

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#define REG_STATUS 0x00
#define REG_MODE 0x01
#define REG_SEQ0 0x04
#define REG_GO 0x0c
#define REG_LAST 0x22
#define SEQ_SLOTS 8u

#define MODE_DEV_RESET 0x80
#define MODE_STANDBY 0x40
#define MODE_MASK 0x07
#define MODE_DIAGNOSTICS 6
#define MODE_AUTO_CAL 7
#define SEQ_WAIT 0x80
#define GO_BIT 0x01

// Table 3's defaults, 00h-22h; STATUS carries DEVICE_ID 7 (DRV2605L).
static const uint8_t defaults[REG_LAST + 1] = {
    0xe0, 0x40, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x05, 0x19, 0xff, 0x19, 0xff, 0x3e, 0x8c, 0x0c, 0x6c, 0x36, 0x93, 0xf5, 0xa0, 0x20, 0x80,
    0x33, 0x00, 0x00,
};

enum {
    PROP_EFFECT_MS,
    PROP_PLAYED,
    PROP_LAST,
    PROP_PLAYING,
};

enum {
    BINDING_I2C,
};

struct drv2605_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    uint32_t effect_ms;
    uint32_t played;
    uint32_t last;
    // The sequencer: the slot playing and when it ends. `go` is a write
    // of GO = 1, taken on the next tick.
    uint8_t slot;
    uint64_t slot_end_us;
    bool playing;
    volatile bool go;
    volatile bool reset;
};

static const struct mclw_property_desc drv2605_properties[] = {
    { .name = "effect_ms", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "ms",
      .min = 1, .max = 10000 },
    { .name = "played", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
    { .name = "last", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 127 },
    { .name = "playing", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = 1 },
};

static const struct mclw_binding_desc drv2605_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static uint8_t get(const struct drv2605_state *s, uint8_t reg) {
    return mclw_regmap_get(&s->map, 0, reg);
}

static void put(struct drv2605_state *s, uint8_t reg, uint8_t value) {
    mclw_regmap_set(&s->map, 0, reg, value, reg == REG_STATUS ? 0 : MCLW_REG_WRITABLE);
}

static void load_defaults(struct drv2605_state *s) {
    for (uint8_t reg = 0; reg <= REG_LAST; reg++) {
        put(s, reg, defaults[reg]);
    }
    s->playing = false;
}

static void MCLW_RAM_FUNC(drv2605_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct drv2605_state *s = ctx;

    (void)map;
    (void)bank;
    if (reg == REG_MODE && (value & MODE_DEV_RESET) != 0) {
        s->reset = true;
    } else if (reg == REG_GO && (value & GO_BIT) != 0) {
        s->go = true;
    }
}

static void finish(struct drv2605_state *s) {
    uint32_t critical = mclw_hal_critical_enter();

    put(s, REG_GO, (uint8_t)(get(s, REG_GO) & ~GO_BIT));
    mclw_hal_critical_exit(critical);
    s->playing = false;
}

// Start the slot at `s->slot`, or finish when it holds 0 or the eight are
// done (Table 8): a waveform for effect_ms, a WAIT for 10 ms per count.
static void start_slot(struct drv2605_state *s, uint64_t now_us) {
    uint8_t v;

    if (s->slot >= SEQ_SLOTS || (v = get(s, (uint8_t)(REG_SEQ0 + s->slot))) == 0) {
        finish(s);
        return;
    }
    if ((v & SEQ_WAIT) != 0) {
        s->slot_end_us = now_us + (uint64_t)(v & 0x7f) * 10000u;
        return;
    }
    s->played++;
    s->last = v;
    s->slot_end_us = now_us + (uint64_t)s->effect_ms * 1000u;
}

static void drv2605_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct drv2605_state *s = inst->state;

    if (s->reset) {
        uint32_t critical = mclw_hal_critical_enter();

        s->reset = false;
        s->go = false;
        load_defaults(s);
        mclw_hal_critical_exit(critical);
        return;
    }
    if (s->playing && (get(s, REG_GO) & GO_BIT) == 0) {
        // Clearing GO cancels playback (Table 9).
        s->playing = false;
    }
    if (!s->playing && s->go) {
        uint8_t mode = get(s, REG_MODE) & MODE_MASK;

        s->go = false;
        s->playing = true;
        if (mode == MODE_DIAGNOSTICS || mode == MODE_AUTO_CAL) {
            // Diagnostics and auto-calibration pass (DIAG_RESULT 0) at once.
            finish(s);
            return;
        }
        s->slot = 0;
        start_slot(s, now_us);
        return;
    }
    if (s->playing && now_us >= s->slot_end_us) {
        s->slot++;
        start_slot(s, now_us);
    }
}

static int drv2605_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct drv2605_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->effect_ms = 100;
    s->played = 0;
    s->last = 0;
    s->go = false;
    s->reset = false;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    load_defaults(s);
    mclw_regmap_set_on_write(&s->map, drv2605_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void drv2605_deinit(struct mclw_instance *inst) {
    struct drv2605_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int drv2605_reset(struct mclw_instance *inst, bool factory) {
    struct drv2605_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    load_defaults(s);
    s->go = false;
    s->reset = false;
    mclw_hal_critical_exit(critical);
    s->played = 0;
    s->last = 0;
    return 0;
}

static int drv2605_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct drv2605_state *s = inst->state;

    switch (property) {
        case PROP_EFFECT_MS:
            out->v[0] = s->effect_ms;
            return 0;
        case PROP_PLAYED:
            out->v[0] = s->played;
            return 0;
        case PROP_LAST:
            out->v[0] = s->last;
            return 0;
        case PROP_PLAYING:
            out->v[0] = s->playing;
            return 0;
        default:
            return -EINVAL;
    }
}

static int drv2605_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct drv2605_state *s = inst->state;

    if (property != PROP_EFFECT_MS) {
        return -EINVAL;
    }
    s->effect_ms = (uint32_t)in->v[0];
    return 0;
}

static const struct mclw_model_ops drv2605_ops = {
    .init = drv2605_init,
    .deinit = drv2605_deinit,
    .reset = drv2605_reset,
    .get = drv2605_get,
    .set = drv2605_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = drv2605_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_drv2605l = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "drv2605l",
    .family = "drv2605",
    .config = NULL,
    .properties = drv2605_properties,
    .property_count = sizeof(drv2605_properties) / sizeof(drv2605_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = drv2605_bindings,
    .binding_count = sizeof(drv2605_bindings) / sizeof(drv2605_bindings[0]),
    .state_size = sizeof(struct drv2605_state),
    .ops = &drv2605_ops,
};
