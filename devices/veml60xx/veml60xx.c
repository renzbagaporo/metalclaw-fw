// SPDX-License-Identifier: Apache-2.0 OR MIT
// Vishay VEML6031X00 (ambient light and IR) and VEML6046X00 (RGB and IR)
// light sensors on I2C: byte registers behind 8-bit command codes, which
// the harness serves from hardware. A measurement runs on the tick for the
// integration time; in active force mode (AF) each trigger takes one and
// flags it in INT_FLAG. Table numbers are the datasheets' (Vishay 80007,
// 80173).

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

#define CHANNELS_MAX 4u
// Table 2: CONF_0, CONF_1, the thresholds, then the data words from 10h.
#define REG_CONF_0 0x00u
#define REG_CONF_1 0x01u
#define REG_DATA 0x10u
// CONF_0 (both parts): ALS_IT/RGB_IT 6:4, AF 3, TRIG 2, INT_EN 1, SD 0.
#define CONF0_AF 0x08u
#define CONF0_TRIG 0x04u
#define CONF0_SD 0x01u
// INT_FLAG's high byte: the active force mode data-ready flag, bit 3.
#define INT_AF_DATA_READY 0x08u

enum {
    PROP_COUNTS,
    PROP_MEASUREMENTS,
};

enum {
    BINDING_I2C,
};

struct veml60xx_part {
    uint8_t channels;
    // The ID word's low byte and the INT_FLAG high byte.
    uint8_t id_reg;
    uint8_t int_reg;
    uint8_t last_reg;
};

struct veml60xx_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    const struct veml60xx_part *part;
    uint16_t counts[CHANNELS_MAX];
    uint32_t measurements;
    uint64_t due_us;
    bool measuring;
    // Set from on_write, taken at the next tick: CONF_0 changed.
    volatile bool configured;
};

// VEML6031: ALS and IR (10h-13h), ID 14h-15h = 0001h, INT_FLAG 16h-17h.
static const struct veml60xx_part veml6031_part = { 2, 0x14, 0x17, 0x17 };
// VEML6046: R, G, B and IR (10h-17h), ID 18h-19h = 0001h, INT_FLAG 1Ah-1Bh.
static const struct veml60xx_part veml6046_part = { 4, 0x18, 0x1b, 0x1b };

static const struct mclw_property_desc veml60xx_properties[] = {
    { .name = "counts", .type = MCLW_TYPE_U32, .count = CHANNELS_MAX, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = 65535 },
    { .name = "measurements", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc veml60xx_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static uint8_t get(const struct veml60xx_state *s, uint8_t reg) {
    return mclw_regmap_get(&s->map, 0, reg);
}

// Table 2's defaults: CONF_0 01h (shut down), CONF_1 80h, the rest 0 but
// the ID; the result and ID registers are read-only, INT_FLAG clears when
// read.
static void reset_registers(struct veml60xx_state *s) {
    const struct veml60xx_part *p = s->part;

    for (uint8_t reg = 0; reg <= p->last_reg; reg++) {
        bool result = reg >= REG_DATA;
        mclw_regmap_set(&s->map, 0, reg, 0, result ? 0 : MCLW_REG_WRITABLE);
    }
    mclw_regmap_set(&s->map, 0, REG_CONF_0, 0x01, MCLW_REG_WRITABLE);
    mclw_regmap_set(&s->map, 0, REG_CONF_1, 0x80, MCLW_REG_WRITABLE);
    mclw_regmap_set(&s->map, 0, p->id_reg, 0x01, 0);
    mclw_regmap_set(&s->map, 0, (uint8_t)(p->int_reg - 1u), 0, MCLW_REG_CLEAR_ON_READ);
    mclw_regmap_set(&s->map, 0, p->int_reg, 0, MCLW_REG_CLEAR_ON_READ);
    s->measuring = false;
    s->configured = true;
}

static void MCLW_RAM_FUNC(veml60xx_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct veml60xx_state *s = ctx;

    (void)map;
    (void)bank;
    (void)value;
    if (reg == REG_CONF_0) {
        s->configured = true;
    }
}

// The integration time of CONF_0's IT field: 3.125 ms x 2^IT.
static uint32_t integration_us(const struct veml60xx_state *s) {
    return 3125u << ((get(s, REG_CONF_0) >> 4) & 7u);
}

// A measurement's result into the data words, little-endian; in AF mode
// the trigger clears and the data-ready flag sets.
static void complete(struct veml60xx_state *s) {
    uint8_t conf = get(s, REG_CONF_0);

    for (uint8_t c = 0; c < s->part->channels; c++) {
        mclw_regmap_set_le16(&s->map, 0, (uint8_t)(REG_DATA + 2u * c), s->counts[c]);
    }
    s->measurements++;
    if ((conf & CONF0_AF) != 0) {
        mclw_regmap_set(&s->map, 0, REG_CONF_0, (uint8_t)(conf & ~CONF0_TRIG), MCLW_REG_WRITABLE);
        mclw_regmap_set(&s->map, 0, s->part->int_reg, (uint8_t)(get(s, s->part->int_reg) | INT_AF_DATA_READY),
            MCLW_REG_CLEAR_ON_READ);
    }
}

static int veml60xx_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct veml60xx_state *s = inst->state;

    s->part = inst->model->config;
    s->i2c = bindings[BINDING_I2C].i2c;
    for (uint8_t c = 0; c < CHANNELS_MAX; c++) {
        s->counts[c] = 1000;
    }
    s->measurements = 0;
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    mclw_regmap_set_on_write(&s->map, veml60xx_on_write, s);
    uint32_t critical = mclw_hal_critical_enter();
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void veml60xx_deinit(struct mclw_instance *inst) {
    struct veml60xx_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int veml60xx_reset(struct mclw_instance *inst, bool factory) {
    struct veml60xx_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    s->measurements = 0;
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int veml60xx_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct veml60xx_state *s = inst->state;

    switch (property) {
        case PROP_COUNTS:
            for (uint8_t c = 0; c < CHANNELS_MAX; c++) {
                out->v[c] = s->counts[c];
            }
            return 0;
        case PROP_MEASUREMENTS:
            out->v[0] = s->measurements;
            return 0;
        default:
            return -EINVAL;
    }
}

static int veml60xx_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct veml60xx_state *s = inst->state;

    if (property != PROP_COUNTS) {
        return -EINVAL;
    }
    for (uint8_t c = 0; c < CHANNELS_MAX && c < in->count; c++) {
        s->counts[c] = (uint16_t)in->v[c];
    }
    return 0;
}

// SD stops measuring; AF takes one measurement per TRIG; otherwise the
// part measures continuously, a result every integration time.
static void veml60xx_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct veml60xx_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();
    uint8_t conf = get(s, REG_CONF_0);
    bool af = (conf & CONF0_AF) != 0;
    bool wanted = (conf & CONF0_SD) == 0 && (!af || (conf & CONF0_TRIG) != 0);

    if (s->configured) {
        s->configured = false;
        s->measuring = wanted;
        s->due_us = now_us + integration_us(s);
    }
    if (s->measuring && now_us >= s->due_us) {
        complete(s);
        s->measuring = !af && wanted;
        s->due_us = now_us + integration_us(s);
    }
    mclw_hal_critical_exit(critical);
}

static const struct mclw_model_ops veml60xx_ops = {
    .init = veml60xx_init,
    .deinit = veml60xx_deinit,
    .reset = veml60xx_reset,
    .get = veml60xx_get,
    .set = veml60xx_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = veml60xx_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

#define VEML60XX_MODEL(symbol, name, part_config) \
    const struct mclw_model symbol = { \
        .abi_version = MCLW_ABI_VERSION, \
        .id = name, \
        .family = "veml60xx", \
        .config = &part_config, \
        .properties = veml60xx_properties, \
        .property_count = sizeof(veml60xx_properties) / sizeof(veml60xx_properties[0]), \
        .regions = NULL, \
        .region_count = 0, \
        .faults = NULL, \
        .fault_count = 0, \
        .bindings = veml60xx_bindings, \
        .binding_count = sizeof(veml60xx_bindings) / sizeof(veml60xx_bindings[0]), \
        .state_size = sizeof(struct veml60xx_state), \
        .ops = &veml60xx_ops, \
    }

VEML60XX_MODEL(mclw_model_veml6031, "veml6031", veml6031_part);
VEML60XX_MODEL(mclw_model_veml6046, "veml6046", veml6046_part);
