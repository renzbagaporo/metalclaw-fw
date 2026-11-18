// SPDX-License-Identifier: Apache-2.0 OR MIT
// Texas Instruments digital temperature sensors on I2C: TMP108, TMP112 and
// TMP116, three models of one register shape. Each register is 16 bits,
// MSB first, behind its pointer value; the temperature converts on the
// part's own schedule, and the limits drive the optional ALERT pin in
// comparator mode. Register and bit names are the datasheets' (TI SBOS663A,
// SBOS473, SNOSD68).

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "tmp1xx.h"

#define REGS_MAX 16u

enum {
    PROP_TEMPERATURE,
    PROP_CONVERSIONS,
};

enum {
    BINDING_I2C,
    BINDING_ALERT,
};

struct tmp1xx_reg {
    uint8_t pointer;
    uint16_t reset;
    // The bits the bus may write; the rest keep what the part holds.
    uint16_t writable;
};

struct tmp1xx_part {
    enum tmp1xx_kind kind;
    const struct tmp1xx_reg *registers;
    uint8_t register_count;
};

struct tmp1xx_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    struct mclw_gpio *alert;
    const struct tmp1xx_part *part;
    uint8_t pointers[MCLW_REGMAP_BANK_SIZE];
    // What each register holds as the part's: the bits the bus may not
    // write come back from here when it writes the register.
    uint16_t held[REGS_MAX];
    bool alert_active;
    int32_t temperature_mc;
    uint32_t conversions;
    // The next conversion's end, and whether one is running at all.
    uint64_t due_us;
    bool scheduled;
    // Set from on_write, taken at the next tick: the schedule restarts.
    volatile bool reschedule;
    // A TMP112 one-shot asked for in shutdown, until its conversion ends.
    volatile bool os_pending;
};

static const struct tmp1xx_reg tmp108_registers[] = {
    { TMP1XX_PTR_TEMP, 0x0000, 0x0000 },
    // Table 8: CR 01 (1 Hz), M 10 (continuous); HYS 01. ID and the flags are the part's.
    { TMP1XX_PTR_CONFIG, 0x2210, 0x67b0 },
    { TMP1XX_PTR_TLOW, 0x0000, 0xfff0 },
    { TMP1XX_PTR_THIGH, 0x0000, 0xfff0 },
};

static const struct tmp1xx_reg tmp112_registers[] = {
    { TMP1XX_PTR_TEMP, 0x0000, 0x0000 },
    // 60A0h: R1:R0 11 (read-only), CR 10 (4 Hz). AL reads the comparator.
    { TMP1XX_PTR_CONFIG, 0x60a0, 0x9fd0 },
    { TMP1XX_PTR_TLOW, 0x4b00, 0xfff1 },
    { TMP1XX_PTR_THIGH, 0x5000, 0xfff1 },
};

static const struct tmp1xx_reg tmp116_registers[] = {
    { TMP1XX_PTR_TEMP, 0x0000, 0x0000 },
    // 0220h: MOD 00 (continuous), CONV 100, AVG 01. The flags are the part's.
    { TMP1XX_PTR_CONFIG, 0x0220, 0x0ffc },
    { TMP116_PTR_THIGH, 0x6000, 0xffff },
    { TMP116_PTR_TLOW, 0x8000, 0xffff },
    { TMP116_PTR_EEPROM_UL, 0x0000, 0x8000 },
    // The factory EEPROM's general-purpose words, here a fixed pattern.
    { TMP116_PTR_EEPROM1, 0x5a5a, 0xffff },
    { TMP116_PTR_EEPROM2, 0x0116, 0xffff },
    { TMP116_PTR_EEPROM3, 0xc0de, 0xffff },
    { TMP116_PTR_EEPROM4, 0x1234, 0xffff },
    { TMP116_PTR_DEVICE_ID, 0x1116, 0x0000 },
};

static const struct tmp1xx_part tmp108_part = { TMP1XX_TMP108, tmp108_registers, 4 };
static const struct tmp1xx_part tmp112_part = { TMP1XX_TMP112, tmp112_registers, 4 };
static const struct tmp1xx_part tmp116_part = { TMP1XX_TMP116, tmp116_registers, 10 };

static const struct mclw_property_desc tmp1xx_properties[] = {
    { .name = "temperature", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mC",
      .min = -55000, .max = 150000 },
    { .name = "conversions", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc tmp1xx_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
    {
        .name = "alert",
        .kind = MCLW_BINDING_GPIO_OUT,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static int MCLW_RAM_FUNC(index_of)(const struct tmp1xx_part *part, uint8_t pointer) {
    for (uint8_t i = 0; i < part->register_count; i++) {
        if (part->registers[i].pointer == pointer) {
            return i;
        }
    }
    return -1;
}

static uint16_t MCLW_RAM_FUNC(get_reg)(struct tmp1xx_state *s, uint8_t pointer) {
    uint8_t at = s->pointers[pointer];

    return (uint16_t)((mclw_regmap_get(&s->map, 0, at) << 8) | mclw_regmap_get(&s->map, 0, (uint8_t)(at + 1u)));
}

static void MCLW_RAM_FUNC(put_reg)(struct tmp1xx_state *s, uint8_t pointer, uint16_t value) {
    int i = index_of(s->part, pointer);
    uint8_t at = s->pointers[pointer];
    uint8_t flags = i >= 0 && s->part->registers[i].writable != 0 ? MCLW_REG_WRITABLE : 0;

    mclw_regmap_set(&s->map, 0, at, (uint8_t)(value >> 8), flags);
    mclw_regmap_set(&s->map, 0, (uint8_t)(at + 1u), (uint8_t)value, flags);
    if (i >= 0) {
        s->held[i] = value;
    }
}

// The temperature in the result register's format: TMP108 and TMP112
// left-justified 12 bits of 0.0625 C (TMP112 13 bits with EM, bit 0 set);
// TMP116 16 bits of 7.8125 m C.
static uint16_t encode(const struct tmp1xx_state *s, int32_t mc, bool extended) {
    if (s->part->kind == TMP1XX_TMP116) {
        int32_t counts = (mc * 128 + (mc >= 0 ? 500 : -500)) / 1000;
        return (uint16_t)(int16_t)(counts > INT16_MAX ? INT16_MAX : counts < INT16_MIN ? INT16_MIN : counts);
    }
    int32_t counts = (mc * 16 + (mc >= 0 ? 500 : -500)) / 1000;
    if (extended) {
        return (uint16_t)((uint16_t)(counts << 3) | 1u);
    }
    return (uint16_t)(counts << 4);
}

static int32_t decode_limit(const struct tmp1xx_state *s, uint16_t v) {
    if (s->part->kind == TMP1XX_TMP116) {
        return (int32_t)(int16_t)v * 1000 / 128;
    }
    if (s->part->kind == TMP1XX_TMP112 && (get_reg((struct tmp1xx_state *)s, TMP1XX_PTR_CONFIG) & TMP112_EM) != 0) {
        return ((int32_t)(int16_t)v >> 3) * 1000 / 16;
    }
    return ((int32_t)(int16_t)v >> 4) * 1000 / 16;
}

// The time from a conversion's start to its result, and between results
// in continuous mode. TMP108: 27 ms, CR 0.25/1/4/16 Hz. TMP112: 26 ms, CR
// 0.25/1/4/8 Hz. TMP116: the conversion cycle time of CONV and AVG, the
// longer of the cycle and the averaging time.
static uint32_t period_us(struct tmp1xx_state *s) {
    uint16_t config = get_reg(s, TMP1XX_PTR_CONFIG);

    switch (s->part->kind) {
        case TMP1XX_TMP108: {
            static const uint32_t rate[] = { 4000000, 1000000, 250000, 62500 };
            return rate[(config >> 13) & 3u];
        }
        case TMP1XX_TMP112: {
            static const uint32_t rate[] = { 4000000, 1000000, 250000, 125000 };
            return rate[(config >> 6) & 3u];
        }
        case TMP1XX_TMP116:
        default: {
            static const uint32_t cycle[] = { 15500, 125000, 250000, 500000, 1000000, 4000000, 8000000, 16000000 };
            static const uint32_t average[] = { 15500, 125000, 500000, 1000000 };
            uint32_t c = cycle[(config >> 7) & 7u];
            uint32_t a = average[(config >> 5) & 3u];
            return c > a ? c : a;
        }
    }
}

static uint32_t conversion_us(const struct tmp1xx_state *s) {
    return s->part->kind == TMP1XX_TMP108 ? 27000u : s->part->kind == TMP1XX_TMP112 ? 26000u : 15500u;
}

// Whether the part converts now, and whether it stops after this one.
static bool running(struct tmp1xx_state *s, bool *one_shot) {
    uint16_t config = get_reg(s, TMP1XX_PTR_CONFIG);

    *one_shot = false;
    switch (s->part->kind) {
        case TMP1XX_TMP108: {
            uint16_t m = (uint16_t)((config >> 8) & 3u);
            *one_shot = m == 1u;
            return m != 0u;
        }
        case TMP1XX_TMP112:
            // SD stops it; OS in shutdown takes one conversion.
            *one_shot = (config & TMP112_SD) != 0;
            return (config & TMP112_SD) == 0 || s->os_pending;
        case TMP1XX_TMP116:
        default: {
            uint16_t mod = (uint16_t)((config >> 10) & 3u);
            *one_shot = mod == 3u;
            return mod != 1u;
        }
    }
}

// A conversion's result: the temperature, the comparator against the
// limits (with the TMP108's hysteresis on the way back), its flags, the
// TMP116's DATA_READY, and the ALERT pin, active low unless POL.
static void complete(struct tmp1xx_state *s, bool one_shot) {
    uint16_t config = get_reg(s, TMP1XX_PTR_CONFIG);
    bool extended = s->part->kind == TMP1XX_TMP112 && (config & TMP112_EM) != 0;
    uint8_t low_ptr = s->part->kind == TMP1XX_TMP116 ? TMP116_PTR_TLOW : TMP1XX_PTR_TLOW;
    uint8_t high_ptr = s->part->kind == TMP1XX_TMP116 ? TMP116_PTR_THIGH : TMP1XX_PTR_THIGH;
    int32_t low = decode_limit(s, get_reg(s, low_ptr));
    int32_t high = decode_limit(s, get_reg(s, high_ptr));
    bool above = s->temperature_mc >= high;
    bool below = s->temperature_mc <= low;
    bool active;
    bool pol;

    put_reg(s, TMP1XX_PTR_TEMP, encode(s, s->temperature_mc, extended));
    s->conversions++;
    switch (s->part->kind) {
        case TMP1XX_TMP108: {
            static const int32_t hys[] = { 0, 1000, 2000, 4000 };
            int32_t h = hys[(config >> 4) & 3u];
            bool was_high = (config & TMP108_FH) != 0;
            bool was_low = (config & TMP108_FL) != 0;
            bool fh = above || (was_high && s->temperature_mc > high - h);
            bool fl = below || (was_low && s->temperature_mc < low + h);

            config = (uint16_t)((config & ~(TMP108_FH | TMP108_FL)) | (fh ? TMP108_FH : 0u) | (fl ? TMP108_FL : 0u));
            if (one_shot) {
                config &= (uint16_t)~0x0300u;
            }
            active = fh || fl;
            pol = (config & TMP108_POL) != 0;
            break;
        }
        case TMP1XX_TMP112:
            // Comparator: active at THIGH, until the temperature falls to TLOW.
            active = above || (s->alert_active && !below);
            pol = (config & TMP112_POL) != 0;
            // AL reads the comparator at the POL level.
            config = (uint16_t)((config & ~TMP112_AL) | ((active == pol) ? TMP112_AL : 0u));
            // OS reads 1 once a one-shot's conversion is done.
            if (one_shot) {
                config = (uint16_t)(config | TMP112_OS);
                s->os_pending = false;
            }
            break;
        case TMP1XX_TMP116:
        default:
            config = (uint16_t)((config & ~(TMP116_HIGH_ALERT | TMP116_LOW_ALERT)) | (above ? TMP116_HIGH_ALERT : 0u) |
                (below ? TMP116_LOW_ALERT : 0u) | TMP116_DATA_READY);
            if (one_shot) {
                config = (uint16_t)((config & ~0x0c00u) | 0x0400u);
            }
            active = above || below;
            pol = (config & TMP116_POL) != 0;
            break;
    }
    put_reg(s, TMP1XX_PTR_CONFIG, config);
    s->alert_active = active;
    if (s->alert != NULL) {
        mclw_hal_gpio_write(s->alert, active == pol);
    }
}

// A register takes its write once its second byte lands: the bits the bus
// may not write keep what the part holds, a locked TMP116 EEPROM word
// keeps its value (EEPROM_UL EUN), and a configuration write restarts the
// conversion schedule.
static void MCLW_RAM_FUNC(tmp1xx_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t addr, uint8_t value,
    void *ctx) {
    struct tmp1xx_state *s = ctx;
    const struct tmp1xx_part *part = s->part;

    (void)map;
    (void)bank;
    (void)value;
    for (uint8_t i = 0; i < part->register_count; i++) {
        const struct tmp1xx_reg *r = &part->registers[i];

        if (r->writable == 0 || addr != (uint8_t)(s->pointers[r->pointer] + 1u)) {
            continue;
        }
        uint16_t v = (uint16_t)((get_reg(s, r->pointer) & r->writable) | (s->held[i] & ~r->writable));
        if (part->kind == TMP1XX_TMP116 && r->pointer >= TMP116_PTR_EEPROM1 && r->pointer <= TMP116_PTR_EEPROM4 &&
            (get_reg(s, TMP116_PTR_EEPROM_UL) & 0x8000u) == 0) {
            v = s->held[i];
        }
        // A TMP112 one-shot: OS reads 0 while it converts.
        if (part->kind == TMP1XX_TMP112 && r->pointer == TMP1XX_PTR_CONFIG && (v & TMP112_SD) != 0 &&
            (v & TMP112_OS) != 0) {
            s->os_pending = true;
            v = (uint16_t)(v & ~TMP112_OS);
        }
        put_reg(s, r->pointer, v);
        if (r->pointer == TMP1XX_PTR_CONFIG) {
            s->reschedule = true;
        }
        return;
    }
}

static void layout(struct tmp1xx_state *s) {
    const struct tmp1xx_part *part = s->part;
    uint8_t spare = (uint8_t)(2u * part->register_count);

    mclw_regmap_init(&s->map, 1, 0, 0x00);
    for (size_t p = 0; p < MCLW_REGMAP_BANK_SIZE; p++) {
        s->pointers[p] = spare;
    }
    for (uint8_t i = 0; i < part->register_count; i++) {
        s->pointers[part->registers[i].pointer] = (uint8_t)(2u * i);
    }
    mclw_regmap_set_pointer_map(&s->map, s->pointers);
    mclw_regmap_set_on_write(&s->map, tmp1xx_on_write, s);
}

static void reset_registers(struct tmp1xx_state *s) {
    for (uint8_t i = 0; i < s->part->register_count; i++) {
        put_reg(s, s->part->registers[i].pointer, s->part->registers[i].reset);
    }
    s->scheduled = false;
    s->reschedule = true;
    s->os_pending = false;
    s->alert_active = false;
}

static int tmp1xx_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct tmp1xx_state *s = inst->state;

    s->part = inst->model->config;
    s->i2c = bindings[BINDING_I2C].i2c;
    s->alert = bindings[BINDING_ALERT].present ? bindings[BINDING_ALERT].gpio : NULL;
    s->temperature_mc = 25000;
    s->conversions = 0;
    layout(s);
    uint32_t critical = mclw_hal_critical_enter();
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    if (s->alert != NULL) {
        mclw_hal_gpio_write(s->alert, true);
    }
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void tmp1xx_deinit(struct mclw_instance *inst) {
    struct tmp1xx_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
    s->alert = NULL;
}

static int tmp1xx_reset(struct mclw_instance *inst, bool factory) {
    struct tmp1xx_state *s = inst->state;
    uint32_t critical;

    (void)factory;
    s->conversions = 0;
    critical = mclw_hal_critical_enter();
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int tmp1xx_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct tmp1xx_state *s = inst->state;

    switch (property) {
        case PROP_TEMPERATURE:
            out->v[0] = s->temperature_mc;
            return 0;
        case PROP_CONVERSIONS:
            out->v[0] = s->conversions;
            return 0;
        default:
            return -EINVAL;
    }
}

static int tmp1xx_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct tmp1xx_state *s = inst->state;

    if (property == PROP_TEMPERATURE) {
        s->temperature_mc = (int32_t)in->v[0];
        return 0;
    }
    return -EINVAL;
}

// The conversion schedule on the tick: a write to the configuration
// register restarts it, so a one-shot or a new rate starts from the write.
static void tmp1xx_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct tmp1xx_state *s = inst->state;
    bool one_shot;
    uint32_t critical = mclw_hal_critical_enter();

    if (s->reschedule) {
        s->reschedule = false;
        s->scheduled = running(s, &one_shot);
        s->due_us = now_us + conversion_us(s);
    }
    if (s->scheduled && now_us >= s->due_us) {
        bool run = running(s, &one_shot);

        if (run) {
            complete(s, one_shot);
            s->scheduled = !one_shot;
            s->due_us = now_us + period_us(s);
        } else {
            s->scheduled = false;
        }
    }
    mclw_hal_critical_exit(critical);
}

static const struct mclw_model_ops tmp1xx_ops = {
    .init = tmp1xx_init,
    .deinit = tmp1xx_deinit,
    .reset = tmp1xx_reset,
    .get = tmp1xx_get,
    .set = tmp1xx_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = tmp1xx_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

#define TMP1XX_MODEL(symbol, name, part_config) \
    const struct mclw_model symbol = { \
        .abi_version = MCLW_ABI_VERSION, \
        .id = name, \
        .family = "tmp1xx", \
        .config = &part_config, \
        .properties = tmp1xx_properties, \
        .property_count = sizeof(tmp1xx_properties) / sizeof(tmp1xx_properties[0]), \
        .regions = NULL, \
        .region_count = 0, \
        .faults = NULL, \
        .fault_count = 0, \
        .bindings = tmp1xx_bindings, \
        .binding_count = sizeof(tmp1xx_bindings) / sizeof(tmp1xx_bindings[0]), \
        .state_size = sizeof(struct tmp1xx_state), \
        .ops = &tmp1xx_ops, \
    }

TMP1XX_MODEL(mclw_model_tmp108, "tmp108", tmp108_part);
TMP1XX_MODEL(mclw_model_tmp112, "tmp112", tmp112_part);
TMP1XX_MODEL(mclw_model_tmp116, "tmp116", tmp116_part);
