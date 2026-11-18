// SPDX-License-Identifier: Apache-2.0 OR MIT
// Sensirion SHT3x-DIS and SHT4x humidity and temperature sensors on I2C.
// Both take commands, not registers: the host writes a command (16 bits on
// the SHT3x, 8 on the SHT4x) and reads the answer, each 16-bit word
// followed by its CRC-8. The harness serves them as a register map whose
// pointer map sends every command's first byte to the answer it reads: the
// measurement, or the status or serial number for the commands that read
// those. Section numbers are the datasheets' (SHT3x-DIS v7, SHT4x v6.6).

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

// Image layout: the measurement (T, CRC, RH, CRC), the status word (SHT3x
// 4.11), the serial number (two words), and a byte nothing selects.
#define AT_MEASUREMENT 0u
#define AT_STATUS 8u
#define AT_SERIAL 12u
#define AT_NONE 20u

enum sht_kind {
    SHT_3X,
    SHT_4X,
};

enum {
    PROP_TEMPERATURE,
    PROP_HUMIDITY,
    PROP_COMMANDS,
};

enum {
    BINDING_I2C,
};

struct sht_part {
    enum sht_kind kind;
    uint32_t serial;
};

struct sht_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    const struct sht_part *part;
    uint8_t pointers[MCLW_REGMAP_BANK_SIZE];
    int32_t temperature_mc;
    int32_t humidity_mpct;
    volatile uint32_t commands;
};

static const struct sht_part sht3x_part = { SHT_3X, 0x0b1e5a7du };
static const struct sht_part sht4x_part = { SHT_4X, 0x0c0ffee4u };

static const struct mclw_property_desc sht_properties[] = {
    { .name = "temperature", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mC",
      .min = -40000, .max = 125000 },
    { .name = "humidity", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "m%RH",
      .min = 0, .max = 100000 },
    { .name = "commands", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc sht_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

// SHT3x 4.12, SHT4x 4.4: CRC-8, polynomial 31h, initial value FFh, over the
// word's two bytes, MSB first.
static uint8_t MCLW_RAM_FUNC(crc8)(uint16_t word) {
    uint8_t crc = 0xff;
    uint8_t bytes[2] = { (uint8_t)(word >> 8), (uint8_t)word };

    for (int i = 0; i < 2; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (uint8_t)((crc & 0x80u) != 0 ? (uint8_t)(crc << 1) ^ 0x31u : (uint8_t)(crc << 1));
        }
    }
    return crc;
}

static void MCLW_RAM_FUNC(put_word)(struct sht_state *s, uint8_t at, uint16_t word) {
    mclw_regmap_set(&s->map, 0, at, (uint8_t)(word >> 8), MCLW_REG_WRITABLE);
    mclw_regmap_set(&s->map, 0, (uint8_t)(at + 1u), (uint8_t)word, MCLW_REG_WRITABLE);
    mclw_regmap_set(&s->map, 0, (uint8_t)(at + 2u), crc8(word), MCLW_REG_WRITABLE);
}

static uint16_t MCLW_RAM_FUNC(scale)(int64_t v, int64_t lo, int64_t span) {
    int64_t raw = ((v - lo) * 65535 + span / 2) / span;

    return (uint16_t)(raw < 0 ? 0 : raw > 65535 ? 65535 : raw);
}

// Every answer as the part would give it now. SHT3x 4.13 and SHT4x 4.6:
// T = -45 + 175 x raw / 65535; RH = 100 x raw / 65535 on the SHT3x,
// -6 + 125 x raw / 65535 on the SHT4x.
static void MCLW_RAM_FUNC(refresh)(struct sht_state *s) {
    bool sht4 = s->part->kind == SHT_4X;

    put_word(s, AT_MEASUREMENT, scale(s->temperature_mc, -45000, 175000));
    put_word(s, AT_MEASUREMENT + 3u, sht4 ? scale(s->humidity_mpct, -6000, 125000) : scale(s->humidity_mpct, 0, 100000));
    put_word(s, AT_STATUS, 0x0000);
    put_word(s, AT_SERIAL, (uint16_t)(s->part->serial >> 16));
    put_word(s, AT_SERIAL + 3u, (uint16_t)s->part->serial);
}

// A command's second byte lands in the image where its first byte points;
// the answers are put back at once, before the read that follows.
static void MCLW_RAM_FUNC(sht_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct sht_state *s = ctx;

    (void)map;
    (void)bank;
    (void)reg;
    (void)value;
    s->commands = s->commands + 1u;
    refresh(s);
}

// SHT3x table 9-13 and SHT4x table 7: the serial number reads (3780h on
// the SHT3x, 89h on the SHT4x) and the status read (F32Dh) answer those;
// every measurement and fetch command reads the measurement.
static void layout(struct sht_state *s) {
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    for (size_t p = 0; p < MCLW_REGMAP_BANK_SIZE; p++) {
        s->pointers[p] = AT_MEASUREMENT;
    }
    if (s->part->kind == SHT_3X) {
        s->pointers[0x37] = AT_SERIAL;
        s->pointers[0xf3] = AT_STATUS;
    } else {
        s->pointers[0x89] = AT_SERIAL;
    }
    mclw_regmap_set_pointer_map(&s->map, s->pointers);
    mclw_regmap_set_on_write(&s->map, sht_on_write, s);
}

static int sht_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct sht_state *s = inst->state;

    s->part = inst->model->config;
    s->i2c = bindings[BINDING_I2C].i2c;
    s->temperature_mc = 25000;
    s->humidity_mpct = 50000;
    s->commands = 0;
    layout(s);
    uint32_t critical = mclw_hal_critical_enter();
    refresh(s);
    mclw_hal_critical_exit(critical);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void sht_deinit(struct mclw_instance *inst) {
    struct sht_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int sht_reset(struct mclw_instance *inst, bool factory) {
    struct sht_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    s->commands = 0;
    refresh(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int sht_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct sht_state *s = inst->state;

    switch (property) {
        case PROP_TEMPERATURE:
            out->v[0] = s->temperature_mc;
            return 0;
        case PROP_HUMIDITY:
            out->v[0] = s->humidity_mpct;
            return 0;
        case PROP_COMMANDS:
            out->v[0] = s->commands;
            return 0;
        default:
            return -EINVAL;
    }
}

static int sht_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct sht_state *s = inst->state;
    uint32_t critical;

    switch (property) {
        case PROP_TEMPERATURE:
            s->temperature_mc = (int32_t)in->v[0];
            break;
        case PROP_HUMIDITY:
            s->humidity_mpct = (int32_t)in->v[0];
            break;
        default:
            return -EINVAL;
    }
    critical = mclw_hal_critical_enter();
    refresh(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static const struct mclw_model_ops sht_ops = {
    .init = sht_init,
    .deinit = sht_deinit,
    .reset = sht_reset,
    .get = sht_get,
    .set = sht_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

#define SHT_MODEL(symbol, name, part_config) \
    const struct mclw_model symbol = { \
        .abi_version = MCLW_ABI_VERSION, \
        .id = name, \
        .family = "sht", \
        .config = &part_config, \
        .properties = sht_properties, \
        .property_count = sizeof(sht_properties) / sizeof(sht_properties[0]), \
        .regions = NULL, \
        .region_count = 0, \
        .faults = NULL, \
        .fault_count = 0, \
        .bindings = sht_bindings, \
        .binding_count = sizeof(sht_bindings) / sizeof(sht_bindings[0]), \
        .state_size = sizeof(struct sht_state), \
        .ops = &sht_ops, \
    }

SHT_MODEL(mclw_model_sht3x, "sht3x", sht3x_part);
SHT_MODEL(mclw_model_sht4x, "sht4x", sht4x_part);
