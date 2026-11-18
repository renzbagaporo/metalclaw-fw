// SPDX-License-Identifier: Apache-2.0 OR MIT
// ST HTS221 humidity and temperature sensor on I2C: Table 15's registers as
// a register map the harness serves from hardware, with a fixed set of
// calibration coefficients and outputs computed from them. Data sheet
// DocID026333 Rev 4; the coefficient layout of Table 19 (on a page the copy
// at hand lacks) as ST's hal_st hts221_reg.h and Zephyr's driver read it.

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

// Table 15.
#define REG_WHO_AM_I 0x0f
#define REG_AV_CONF 0x10
#define REG_CTRL_REG1 0x20
#define REG_CTRL_REG2 0x21
#define REG_CTRL_REG3 0x22
#define REG_STATUS 0x27
#define REG_HUMIDITY_OUT_L 0x28
#define REG_TEMP_OUT_L 0x2a
#define REG_CALIB 0x30

#define WHO_AM_I 0xbc
#define AV_CONF_RESET 0x1b
#define CTRL1_PD 0x80
#define CTRL1_ODR 0x03
#define CTRL2_BOOT 0x80
#define CTRL2_ONE_SHOT 0x01
#define STATUS_H_DA 0x02
#define STATUS_T_DA 0x01

// The coefficients this part carries: 33.0 %RH at count 0 and 75.0 %RH at
// 8400; 20.0 C at count 0 and 35.0 C at 1200. A real part's differ; the
// driver reads them at boot.
#define H0_RH_X2 66
#define H1_RH_X2 150
#define T0_DEGC_X8 160
#define T1_DEGC_X8 280
#define H0_T0_OUT 0
#define H1_T0_OUT 8400
#define T0_OUT 0
#define T1_OUT 1200

enum {
    PROP_TEMPERATURE,
    PROP_HUMIDITY,
};

enum {
    BINDING_I2C,
};

struct hts221_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    uint8_t pointers[256];
    int32_t temperature_mc;
    int32_t humidity_mpct;
    uint64_t next_us;
    volatile bool one_shot;
};

static const struct mclw_property_desc hts221_properties[] = {
    { .name = "temperature", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mC",
      .min = -40000, .max = 120000 },
    { .name = "humidity", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "m%RH",
      .min = 0, .max = 100000 },
};

static const struct mclw_binding_desc hts221_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static uint8_t get(const struct hts221_state *s, uint8_t reg) {
    return mclw_regmap_get(&s->map, 0, reg);
}

static void put(struct hts221_state *s, uint8_t reg, uint8_t value, uint8_t flags) {
    mclw_regmap_set(&s->map, 0, reg, value, flags);
}

// Section 8's linear interpolation run backwards: the count that reads
// back as `value` (in thousandths of the x2 or x8 unit), rounded.
static int16_t count_for(int64_t value_milli, int64_t p0, int64_t p1, int64_t c0, int64_t c1) {
    int64_t num = (value_milli - p0 * 1000) * (c1 - c0);
    int64_t den = (p1 - p0) * 1000;
    int64_t c = c0 + (num >= 0 ? (num + den / 2) / den : (num - den / 2) / den);

    if (c > INT16_MAX) {
        c = INT16_MAX;
    } else if (c < INT16_MIN) {
        c = INT16_MIN;
    }
    return (int16_t)c;
}

// HUMIDITY_OUT and TEMP_OUT, two's complement, LSB first, and the
// data-available flags.
static void sample(struct hts221_state *s) {
    uint16_t h = (uint16_t)count_for((int64_t)s->humidity_mpct * 2, H0_RH_X2, H1_RH_X2, H0_T0_OUT, H1_T0_OUT);
    uint16_t t = (uint16_t)count_for((int64_t)s->temperature_mc * 8, T0_DEGC_X8, T1_DEGC_X8, T0_OUT, T1_OUT);
    uint32_t critical = mclw_hal_critical_enter();

    mclw_regmap_set_le16(&s->map, 0, REG_HUMIDITY_OUT_L, h);
    mclw_regmap_set_le16(&s->map, 0, REG_TEMP_OUT_L, t);
    put(s, REG_STATUS, STATUS_H_DA | STATUS_T_DA, 0);
    mclw_hal_critical_exit(critical);
}

static void calibration(struct hts221_state *s) {
    static const uint8_t calib[16] = {
        H0_RH_X2, H1_RH_X2, T0_DEGC_X8 & 0xff, T1_DEGC_X8 & 0xff, 0x00,
        (uint8_t)(((T0_DEGC_X8 >> 8) & 3) | (((T1_DEGC_X8 >> 8) & 3) << 2)),
        H0_T0_OUT & 0xff, (H0_T0_OUT >> 8) & 0xff, 0x00, 0x00,
        H1_T0_OUT & 0xff, (H1_T0_OUT >> 8) & 0xff,
        T0_OUT & 0xff, (T0_OUT >> 8) & 0xff,
        T1_OUT & 0xff, (T1_OUT >> 8) & 0xff,
    };

    for (uint8_t i = 0; i < sizeof(calib); i++) {
        put(s, (uint8_t)(REG_CALIB + i), calib[i], 0);
    }
}

// CTRL_REG2: ONE_SHOT starts a conversion and BOOT a reload; both clear
// themselves when done.
static void MCLW_RAM_FUNC(hts221_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct hts221_state *s = ctx;

    (void)bank;
    if (reg != REG_CTRL_REG2) {
        return;
    }
    if ((value & CTRL2_ONE_SHOT) != 0) {
        s->one_shot = true;
    }
    if ((value & CTRL2_BOOT) != 0) {
        mclw_regmap_set(map, 0, REG_CTRL_REG2, (uint8_t)(value & ~CTRL2_BOOT), MCLW_REG_WRITABLE);
    }
}

static void power_on(struct hts221_state *s) {
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    put(s, REG_WHO_AM_I, WHO_AM_I, 0);
    put(s, REG_AV_CONF, AV_CONF_RESET, MCLW_REG_WRITABLE);
    put(s, REG_CTRL_REG1, 0x00, MCLW_REG_WRITABLE);
    put(s, REG_CTRL_REG2, 0x00, MCLW_REG_WRITABLE);
    put(s, REG_CTRL_REG3, 0x00, MCLW_REG_WRITABLE);
    put(s, REG_STATUS, 0x00, 0);
    mclw_regmap_set_le16(&s->map, 0, REG_HUMIDITY_OUT_L, 0);
    mclw_regmap_set_le16(&s->map, 0, REG_TEMP_OUT_L, 0);
    calibration(s);
    s->next_us = 0;
    s->one_shot = false;
}

static int hts221_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct hts221_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->temperature_mc = 25000;
    s->humidity_mpct = 50000;
    power_on(s);
    // 5.1.1: bit 7 of the sub-address asks for auto-increment; the
    // register is the other seven bits.
    for (size_t p = 0; p < 256; p++) {
        s->pointers[p] = (uint8_t)(p & 0x7f);
    }
    mclw_regmap_set_pointer_map(&s->map, s->pointers);
    mclw_regmap_set_on_write(&s->map, hts221_on_write, s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void hts221_deinit(struct mclw_instance *inst) {
    struct hts221_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int hts221_reset(struct mclw_instance *inst, bool factory) {
    struct hts221_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    (void)factory;
    power_on(s);
    mclw_regmap_set_pointer_map(&s->map, s->pointers);
    mclw_regmap_set_on_write(&s->map, hts221_on_write, s);
    mclw_hal_critical_exit(critical);
    return 0;
}

// Table 17: 1, 7 and 12.5 Hz while PD is set; 00 is one-shot.
static void hts221_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    static const uint32_t period_us[4] = { 0, 1000000u, 142857u, 80000u };
    struct hts221_state *s = inst->state;
    uint8_t ctrl1 = get(s, REG_CTRL_REG1);
    uint32_t period = (ctrl1 & CTRL1_PD) != 0 ? period_us[ctrl1 & CTRL1_ODR] : 0;

    if (s->one_shot) {
        uint32_t critical = mclw_hal_critical_enter();

        s->one_shot = false;
        put(s, REG_CTRL_REG2, (uint8_t)(get(s, REG_CTRL_REG2) & ~CTRL2_ONE_SHOT), MCLW_REG_WRITABLE);
        mclw_hal_critical_exit(critical);
        sample(s);
    }
    if (period == 0) {
        s->next_us = 0;
        return;
    }
    if (s->next_us == 0) {
        s->next_us = now_us + period;
    }
    if (now_us >= s->next_us) {
        s->next_us += period;
        if (s->next_us <= now_us) {
            s->next_us = now_us + period;
        }
        sample(s);
    }
}

static int hts221_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct hts221_state *s = inst->state;

    switch (property) {
        case PROP_TEMPERATURE:
            out->v[0] = s->temperature_mc;
            return 0;
        case PROP_HUMIDITY:
            out->v[0] = s->humidity_mpct;
            return 0;
        default:
            return -EINVAL;
    }
}

static int hts221_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct hts221_state *s = inst->state;

    switch (property) {
        case PROP_TEMPERATURE:
            s->temperature_mc = (int32_t)in->v[0];
            return 0;
        case PROP_HUMIDITY:
            s->humidity_mpct = (int32_t)in->v[0];
            return 0;
        default:
            return -EINVAL;
    }
}

static const struct mclw_model_ops hts221_ops = {
    .init = hts221_init,
    .deinit = hts221_deinit,
    .reset = hts221_reset,
    .get = hts221_get,
    .set = hts221_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = hts221_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_hts221 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "hts221",
    .family = "hts221",
    .config = NULL,
    .properties = hts221_properties,
    .property_count = sizeof(hts221_properties) / sizeof(hts221_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = hts221_bindings,
    .binding_count = sizeof(hts221_bindings) / sizeof(hts221_bindings[0]),
    .state_size = sizeof(struct hts221_state),
    .ops = &hts221_ops,
};
