// SPDX-License-Identifier: Apache-2.0 OR MIT
// Atmel/Microchip AT24C02C 2-Kbit I2C serial EEPROM: 256 bytes behind an
// 8-bit word address, a register map the harness serves from hardware. The
// array keeps what the bus writes across a reset (it is non-volatile), and
// the host reads and writes it as a programmer would. Atmel-8700G-SEEPROM
// (01/2015).

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#define ARRAY_BYTES 256u

enum {
    PROP_WRITES,
};

enum {
    REGION_ARRAY,
};

enum {
    BINDING_I2C,
};

struct at24_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    volatile uint32_t writes;
};

static const struct mclw_property_desc at24_properties[] = {
    { .name = "writes", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_region_desc at24_regions[] = {
    { .name = "array", .size = ARRAY_BYTES, .access = MCLW_ACCESS_RW },
};

static const struct mclw_binding_desc at24_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static void MCLW_RAM_FUNC(at24_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct at24_state *s = ctx;

    (void)map;
    (void)bank;
    (void)reg;
    (void)value;
    s->writes = s->writes + 1u;
}

// A fresh part reads FFh everywhere (erased); every byte takes writes.
static int at24_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct at24_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->writes = 0;
    mclw_regmap_init(&s->map, 1, 0, 0xff);
    for (uint16_t at = 0; at < ARRAY_BYTES; at++) {
        mclw_regmap_set(&s->map, 0, (uint8_t)at, 0xff, MCLW_REG_WRITABLE);
    }
    mclw_regmap_set_on_write(&s->map, at24_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void at24_deinit(struct mclw_instance *inst) {
    struct at24_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

// A power cycle keeps the array; a factory reset erases it.
static int at24_reset(struct mclw_instance *inst, bool factory) {
    struct at24_state *s = inst->state;

    s->writes = 0;
    if (factory) {
        uint32_t critical = mclw_hal_critical_enter();
        for (uint16_t at = 0; at < ARRAY_BYTES; at++) {
            mclw_regmap_set(&s->map, 0, (uint8_t)at, 0xff, MCLW_REG_WRITABLE);
        }
        mclw_hal_critical_exit(critical);
    }
    return 0;
}

static int at24_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct at24_state *s = inst->state;

    if (property == PROP_WRITES) {
        out->v[0] = s->writes;
        return 0;
    }
    return -EINVAL;
}

static int at24_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    (void)inst;
    (void)property;
    (void)in;
    return -EINVAL;
}

static int at24_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    const struct at24_state *s = inst->state;
    uint8_t *out = buf;

    (void)region;
    for (size_t i = 0; i < len; i++) {
        out[i] = mclw_regmap_get(&s->map, 0, (uint8_t)(offset + i));
    }
    return 0;
}

static int at24_write_region(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf,
    size_t len) {
    struct at24_state *s = inst->state;
    const uint8_t *in = buf;
    uint32_t critical = mclw_hal_critical_enter();

    (void)region;
    for (size_t i = 0; i < len; i++) {
        mclw_regmap_set(&s->map, 0, (uint8_t)(offset + i), in[i], MCLW_REG_WRITABLE);
    }
    mclw_hal_critical_exit(critical);
    return 0;
}

static const struct mclw_model_ops at24_ops = {
    .init = at24_init,
    .deinit = at24_deinit,
    .reset = at24_reset,
    .get = at24_get,
    .set = at24_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = at24_read_region,
    .write_region = at24_write_region,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_at24c02 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "at24c02",
    .family = "at24",
    .config = NULL,
    .properties = at24_properties,
    .property_count = sizeof(at24_properties) / sizeof(at24_properties[0]),
    .regions = at24_regions,
    .region_count = sizeof(at24_regions) / sizeof(at24_regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = at24_bindings,
    .binding_count = sizeof(at24_bindings) / sizeof(at24_bindings[0]),
    .state_size = sizeof(struct at24_state),
    .ops = &at24_ops,
};
