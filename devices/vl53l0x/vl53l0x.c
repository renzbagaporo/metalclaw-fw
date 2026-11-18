// SPDX-License-Identifier: Apache-2.0 OR MIT
// ST VL53L0X time-of-flight ranging sensor on I2C, as ST's VL53L0X API
// (hal_st, vl53l0x_api*.c) drives it: ST publishes no register map, so the
// registers and handshakes here are the API's, and the identification is
// what Zephyr's driver checks (EEh AAh at C0h). A ranging (SYSRANGE_START,
// 00h on page 0) completes as soon as it is started, with the range set
// here.

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

// vl53l0x_device.h.
#define REG_SYSRANGE_START 0x00
#define REG_SYSTEM_INTERRUPT_CONFIG_GPIO 0x0a
#define REG_SYSTEM_INTERRUPT_CLEAR 0x0b
#define REG_RESULT_INTERRUPT_STATUS 0x13
#define REG_RESULT_RANGE_STATUS 0x14
#define REG_RESULT_RANGE_MM 0x1e
#define REG_STROBE 0x83
#define REG_NVM_DATA 0x90
#define REG_NVM_COMMAND 0x94
#define REG_PEAK_SIGNAL_RATE_REF 0xb6
#define REG_MODEL_ID 0xc0
#define REG_MODEL_ID_2 0xc1
#define REG_REVISION_ID 0xc2
#define REG_PAGE 0xff

// The API's new-sample-ready interrupt (VL53L0X_REG_SYSTEM_INTERRUPT_GPIO_
// NEW_SAMPLE_READY) and a valid range (status 11, in bits 6:3).
#define INTERRUPT_NEW_SAMPLE 0x04
#define RANGE_VALID (11u << 3)
// NVM command 6Bh: the reference SPAD count (bits 14:8) and type (bit 15),
// read back from 90h-93h (VL53L0X_get_info_from_device).
#define NVM_SPAD_INFO 0x6b
// NVM commands 24h and 25h: the reference SPADs that work (the good SPAD
// map's six bytes, 24h's four then 25h's first two), every one here.
#define NVM_GOOD_SPADS_0 0x24
#define NVM_GOOD_SPADS_1 0x25
#define REF_SPAD_COUNT 5u
#define REF_SPAD_APERTURE 1u
// The reference signal rate the API compares with its 20 MCPS target
// (9.7 fixed point), in reference SPAD management.
#define REF_SIGNAL_RATE 0x0b00u

enum {
    PROP_DISTANCE,
    PROP_RANGINGS,
};

enum {
    BINDING_I2C,
};

struct vl53l0x_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    uint32_t distance_mm;
    uint32_t rangings;
};

static const struct mclw_property_desc vl53l0x_properties[] = {
    { .name = "distance", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mm",
      .min = 0, .max = 2000 },
    { .name = "rangings", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc vl53l0x_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static void be16(struct mclw_regmap *map, uint8_t reg, uint32_t value) {
    mclw_regmap_set(map, 0, reg, (uint8_t)(value >> 8), MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, (uint8_t)(reg + 1u), (uint8_t)value, MCLW_REG_WRITABLE);
}

// A ranging, or one of the API's single reference calibrations: done at
// once, with its result and the new-sample interrupt.
static void range(struct vl53l0x_state *s) {
    struct mclw_regmap *map = &s->map;

    mclw_regmap_set(map, 0, REG_SYSRANGE_START, 0x00, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, REG_RESULT_RANGE_STATUS, (uint8_t)(RANGE_VALID | 0x01u), MCLW_REG_WRITABLE);
    be16(map, REG_RESULT_RANGE_MM, s->distance_mm);
    be16(map, REG_PEAK_SIGNAL_RATE_REF, REF_SIGNAL_RATE);
    mclw_regmap_set(map, 0, REG_RESULT_INTERRUPT_STATUS, INTERRUPT_NEW_SAMPLE, MCLW_REG_WRITABLE);
    s->rangings++;
}

static void nvm(struct mclw_regmap *map, uint8_t command) {
    uint32_t word = 0;

    if (command == NVM_SPAD_INFO) {
        word = (REF_SPAD_APERTURE << 15) | (REF_SPAD_COUNT << 8);
    } else if (command == NVM_GOOD_SPADS_0 || command == NVM_GOOD_SPADS_1) {
        word = 0xffffffffu;
    }
    for (uint8_t i = 0; i < 4; i++) {
        mclw_regmap_set(map, 0, (uint8_t)(REG_NVM_DATA + i), (uint8_t)(word >> (24u - 8u * i)), MCLW_REG_WRITABLE);
    }
}

static void MCLW_RAM_FUNC(vl53l0x_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct vl53l0x_state *s = ctx;
    uint8_t page = mclw_regmap_get(map, 0, REG_PAGE);

    (void)bank;
    if (reg == REG_SYSRANGE_START && page == 0x00 && (value & 0x01u) != 0u) {
        range(s);
    } else if (reg == REG_SYSTEM_INTERRUPT_CLEAR && (value & 0x07u) != 0u) {
        mclw_regmap_set(map, 0, REG_RESULT_INTERRUPT_STATUS, 0x00, MCLW_REG_WRITABLE);
        mclw_regmap_set(map, 0, REG_RESULT_RANGE_STATUS,
            (uint8_t)(mclw_regmap_get(map, 0, REG_RESULT_RANGE_STATUS) & ~0x01u), MCLW_REG_WRITABLE);
    } else if (reg == REG_STROBE && value == 0x00) {
        // The NVM read strobe: ready at once.
        mclw_regmap_set(map, 0, REG_STROBE, 0x01, MCLW_REG_WRITABLE);
    } else if (reg == REG_NVM_COMMAND) {
        nvm(map, value);
    }
}

static void power_on(struct vl53l0x_state *s) {
    struct mclw_regmap *map = &s->map;

    for (uint32_t reg = 0; reg < 0x100u; reg++) {
        mclw_regmap_set(map, 0, (uint8_t)reg, 0x00, MCLW_REG_WRITABLE);
    }
    mclw_regmap_set(map, 0, REG_MODEL_ID, 0xee, 0);
    mclw_regmap_set(map, 0, REG_MODEL_ID_2, 0xaa, 0);
    mclw_regmap_set(map, 0, REG_REVISION_ID, 0x10, 0);
    mclw_regmap_set(map, 0, REG_SYSTEM_INTERRUPT_CONFIG_GPIO, INTERRUPT_NEW_SAMPLE, MCLW_REG_WRITABLE);
}

static int vl53l0x_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct vl53l0x_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->distance_mm = 200;
    s->rangings = 0;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    power_on(s);
    mclw_regmap_set_on_write(&s->map, vl53l0x_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void vl53l0x_deinit(struct mclw_instance *inst) {
    struct vl53l0x_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int vl53l0x_reset(struct mclw_instance *inst, bool factory) {
    struct vl53l0x_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    power_on(s);
    s->rangings = 0;
    mclw_hal_critical_exit(critical);
    return 0;
}

static int vl53l0x_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct vl53l0x_state *s = inst->state;

    switch (property) {
        case PROP_DISTANCE:
            out->v[0] = s->distance_mm;
            return 0;
        case PROP_RANGINGS:
            out->v[0] = s->rangings;
            return 0;
        default:
            return -EINVAL;
    }
}

static int vl53l0x_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct vl53l0x_state *s = inst->state;

    switch (property) {
        case PROP_DISTANCE:
            s->distance_mm = (uint32_t)in->v[0];
            return 0;
        default:
            return -EINVAL;
    }
}

static const struct mclw_model_ops vl53l0x_ops = {
    .init = vl53l0x_init,
    .deinit = vl53l0x_deinit,
    .reset = vl53l0x_reset,
    .get = vl53l0x_get,
    .set = vl53l0x_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_vl53l0x = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "vl53l0x",
    .family = "vl53l0x",
    .config = NULL,
    .properties = vl53l0x_properties,
    .property_count = sizeof(vl53l0x_properties) / sizeof(vl53l0x_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = vl53l0x_bindings,
    .binding_count = sizeof(vl53l0x_bindings) / sizeof(vl53l0x_bindings[0]),
    .state_size = sizeof(struct vl53l0x_state),
    .ops = &vl53l0x_ops,
};
