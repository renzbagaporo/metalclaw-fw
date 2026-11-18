// SPDX-License-Identifier: Apache-2.0 OR MIT
// Maxim DS3231 I2C real-time clock: the timekeeping, alarm, control, status,
// aging and temperature registers (00h-12h) as a register map the harness
// serves from hardware. The clock counts whole seconds from the harness's
// time base, the two alarms match on each second and drive INT/SQW, and with
// INTCN clear the pin carries the 1 Hz square wave. Data sheet 19-5170
// Rev 10 (3/15).

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

// Figure 1.
#define REG_SECONDS 0x00
#define REG_MINUTES 0x01
#define REG_HOURS 0x02
#define REG_DAY 0x03
#define REG_DATE 0x04
#define REG_MONTH 0x05
#define REG_YEAR 0x06
#define REG_A1_SECONDS 0x07
#define REG_A2_MINUTES 0x0b
#define REG_CONTROL 0x0e
#define REG_STATUS 0x0f
#define REG_AGING 0x10
#define REG_TEMP_MSB 0x11
#define REG_TEMP_LSB 0x12
#define REG_LAST REG_TEMP_LSB

#define HOURS_12H 0x40
#define HOURS_PM 0x20
#define MONTH_CENTURY 0x80
#define ALARM_MASK 0x80
#define ALARM_DY 0x40

// Control (0Eh) and status (0Fh), with their power-on values.
#define CONTROL_CONV 0x20
#define CONTROL_RS 0x18
#define CONTROL_INTCN 0x04
#define CONTROL_A2IE 0x02
#define CONTROL_A1IE 0x01
#define CONTROL_POR 0x1c
#define STATUS_OSF 0x80
#define STATUS_EN32KHZ 0x08
#define STATUS_BSY 0x04
#define STATUS_A2F 0x02
#define STATUS_A1F 0x01
#define STATUS_POR (STATUS_OSF | STATUS_EN32KHZ)

#define SECOND_US 1000000u
#define HALF_SECOND_US 500000u

enum {
    PROP_TEMPERATURE,
    PROP_ALARMS,
};

enum {
    BINDING_I2C,
    BINDING_INT,
};

struct ds3231_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    struct mclw_gpio *int_pin;
    int32_t temperature_mc;
    uint32_t alarms[2];
    uint64_t next_second_us;
    // Status as the bus left it; the flags only clear from the bus.
    volatile uint8_t status;
    // Set from the bus: a seconds write restarts the countdown chain, CONV
    // starts a conversion.
    volatile bool chain_reset;
    volatile bool convert;
    volatile bool sqw_high;
    volatile bool pin_low;
};

static const struct mclw_property_desc ds3231_properties[] = {
    { .name = "temperature", .type = MCLW_TYPE_I32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "mC",
      .min = -40000, .max = 85000 },
    { .name = "alarms", .type = MCLW_TYPE_U32, .count = 2, .access = MCLW_ACCESS_RO, .unit = NULL,
      .min = 0, .max = UINT32_MAX },
};

static const struct mclw_binding_desc ds3231_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
    {
        .name = "int",
        .kind = MCLW_BINDING_GPIO_OUT,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static uint8_t get(const struct ds3231_state *s, uint8_t reg) {
    return mclw_regmap_get(&s->map, 0, reg);
}

static void put(struct ds3231_state *s, uint8_t reg, uint8_t value, uint8_t flags) {
    mclw_regmap_set(&s->map, 0, reg, value, flags);
}

static uint8_t bcd(uint8_t v) {
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

static uint8_t bin(uint8_t v) {
    return (uint8_t)((v >> 4) * 10u + (v & 0x0fu));
}

// Hours register to 0-23, in either mode (Figure 1: bit 6 12/24, bit 5 PM
// or 20 hour, bit 4 10 hour).
static uint8_t hours_24(uint8_t reg) {
    if ((reg & HOURS_12H) != 0) {
        uint8_t h = (uint8_t)(bin(reg & 0x1f) % 12u);
        return (uint8_t)(h + (((reg & HOURS_PM) != 0) ? 12u : 0u));
    }
    return bin(reg & 0x3f);
}

static uint8_t hours_reg(uint8_t h24, bool twelve) {
    if (!twelve) {
        return bcd(h24);
    }
    uint8_t h = (uint8_t)(h24 % 12u);
    return (uint8_t)(HOURS_12H | (h24 >= 12u ? HOURS_PM : 0u) | bcd(h == 0 ? 12u : h));
}

// The leap-year compensation is valid to 2100 (Features): every fourth year.
static uint8_t month_days(uint8_t month, uint8_t year) {
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month == 2 && (year % 4u) == 0) {
        return 29;
    }
    return (month >= 1 && month <= 12) ? days[month - 1] : 31;
}

// Table 2. Alarm 1 compares seconds, minutes, hours and day or date; alarm
// 2 the same without seconds, on the second the seconds read 00.
static bool alarm_matches(const struct ds3231_state *s, uint8_t base, bool with_seconds) {
    uint8_t now[4] = {
        get(s, REG_SECONDS),
        get(s, REG_MINUTES),
        get(s, REG_HOURS),
        0,
    };
    uint8_t first = with_seconds ? 0u : 1u;

    if (!with_seconds && (now[0] & 0x7f) != 0) {
        return false;
    }
    for (uint8_t i = first; i < 4u; i++) {
        uint8_t a = get(s, (uint8_t)(base + i - first));

        if ((a & ALARM_MASK) != 0) {
            continue;
        }
        switch (i) {
            case 0:
            case 1:
                if ((a & 0x7f) != (now[i] & 0x7f)) {
                    return false;
                }
                break;
            case 2:
                if (hours_24(a) != hours_24(now[2])) {
                    return false;
                }
                break;
            default:
                if ((a & ALARM_DY) != 0) {
                    if ((a & 0x0f) != (get(s, REG_DAY) & 0x07)) {
                        return false;
                    }
                } else if ((a & 0x3f) != (get(s, REG_DATE) & 0x3f)) {
                    return false;
                }
                break;
        }
    }
    return true;
}

// The once-per-second update: the time and date roll over in BCD, then the
// alarms are tested on the new time.
static uint8_t tick(struct ds3231_state *s) {
    uint8_t sec = bin(get(s, REG_SECONDS) & 0x7f);
    uint8_t min = bin(get(s, REG_MINUTES) & 0x7f);
    uint8_t hours = get(s, REG_HOURS);
    uint8_t h24 = hours_24(hours);
    uint8_t day = get(s, REG_DAY) & 0x07;
    uint8_t date = bin(get(s, REG_DATE) & 0x3f);
    uint8_t month_reg = get(s, REG_MONTH);
    uint8_t month = bin(month_reg & 0x1f);
    uint8_t century = month_reg & MONTH_CENTURY;
    uint8_t year = bin(get(s, REG_YEAR));
    uint8_t flags = 0;

    if (++sec >= 60u) {
        sec = 0;
        if (++min >= 60u) {
            min = 0;
            if (++h24 >= 24u) {
                h24 = 0;
                day = (uint8_t)(day >= 7u ? 1u : day + 1u);
                if (++date > month_days(month, year)) {
                    date = 1;
                    if (++month > 12u) {
                        month = 1;
                        if (++year > 99u) {
                            year = 0;
                            century ^= MONTH_CENTURY;
                        }
                    }
                }
            }
        }
    }
    put(s, REG_SECONDS, bcd(sec), MCLW_REG_WRITABLE);
    put(s, REG_MINUTES, bcd(min), MCLW_REG_WRITABLE);
    put(s, REG_HOURS, hours_reg(h24, (hours & HOURS_12H) != 0), MCLW_REG_WRITABLE);
    put(s, REG_DAY, day, MCLW_REG_WRITABLE);
    put(s, REG_DATE, bcd(date), MCLW_REG_WRITABLE);
    put(s, REG_MONTH, (uint8_t)(century | bcd(month)), MCLW_REG_WRITABLE);
    put(s, REG_YEAR, bcd(year), MCLW_REG_WRITABLE);
    if (alarm_matches(s, REG_A1_SECONDS, true)) {
        flags |= STATUS_A1F;
    }
    if (alarm_matches(s, REG_A2_MINUTES, false)) {
        flags |= STATUS_A2F;
    }
    return flags;
}

static void put_temperature(struct ds3231_state *s) {
    // 11h-12h: a two's complement 10-bit value in 0.25 C, MSB the integer.
    int32_t quarters = s->temperature_mc >= 0 ? (s->temperature_mc + 125) / 250 : (s->temperature_mc - 125) / 250;
    uint16_t raw = (uint16_t)((uint16_t)(int16_t)quarters << 6);

    put(s, REG_TEMP_MSB, (uint8_t)(raw >> 8), 0);
    put(s, REG_TEMP_LSB, (uint8_t)raw, 0);
}

// INT/SQW is open drain, active low: with INTCN set it is low while an
// enabled alarm's flag is set; clear, it carries the RS2-RS1 square wave,
// of which the model makes the 1 Hz one (RS 00) and holds the rest high.
// A control or status write moves the pin from the bus interrupt, as the
// part does on the write's acknowledge: Zephyr's driver arms its edge
// interrupt right after clearing INTCN and takes the next edge as the
// second's, so a pin that changed later would hand it a false one. Called
// under the critical section or from the bus interrupt.
static void MCLW_RAM_FUNC(drive_pin)(struct ds3231_state *s) {
    uint8_t control = get(s, REG_CONTROL);
    bool low;

    if (s->int_pin == NULL) {
        return;
    }
    if ((control & CONTROL_INTCN) != 0) {
        low = (s->status & control & (CONTROL_A1IE | CONTROL_A2IE)) != 0;
    } else {
        low = (control & CONTROL_RS) == 0 && !s->sqw_high;
    }
    if (low == s->pin_low) {
        return;
    }
    s->pin_low = low;
    if (low) {
        mclw_hal_gpio_write(s->int_pin, false);
    } else {
        mclw_hal_gpio_release(s->int_pin);
    }
}

// Status (0Fh): OSF, A2F and A1F only clear from the bus, EN32kHz takes
// writes, BSY and bits 6-4 are read-only.
static void MCLW_RAM_FUNC(ds3231_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value,
    void *ctx) {
    struct ds3231_state *s = ctx;

    (void)bank;
    switch (reg) {
        case REG_SECONDS:
            s->chain_reset = true;
            break;
        case REG_CONTROL:
            if ((value & CONTROL_CONV) != 0) {
                s->convert = true;
            }
            drive_pin(s);
            break;
        case REG_STATUS: {
            uint8_t old = s->status;
            uint8_t next = (uint8_t)((old & value & (STATUS_OSF | STATUS_A2F | STATUS_A1F))
                | (value & STATUS_EN32KHZ) | (old & STATUS_BSY));
            s->status = next;
            mclw_regmap_set(map, 0, REG_STATUS, next, MCLW_REG_WRITABLE);
            drive_pin(s);
            break;
        }
        default:
            break;
    }
}

// Power-on: the time registers are undefined, here 2000-01-01 00:00:00, day
// 1; control 1Ch, status 88h (OSF set: the oscillator has just started).
static void power_on(struct ds3231_state *s) {
    static const uint8_t time[REG_YEAR + 1] = { 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00 };

    for (uint8_t reg = 0; reg <= REG_LAST; reg++) {
        uint8_t value = reg <= REG_YEAR ? time[reg] : 0x00;
        put(s, reg, value, reg >= REG_TEMP_MSB ? 0 : MCLW_REG_WRITABLE);
    }
    put(s, REG_CONTROL, CONTROL_POR, MCLW_REG_WRITABLE);
    s->status = STATUS_POR;
    put(s, REG_STATUS, STATUS_POR, MCLW_REG_WRITABLE);
    put_temperature(s);
    s->alarms[0] = 0;
    s->alarms[1] = 0;
    s->next_second_us = 0;
    s->chain_reset = false;
    s->convert = false;
    s->sqw_high = false;
}

static int ds3231_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct ds3231_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->int_pin = bindings[BINDING_INT].present ? bindings[BINDING_INT].gpio : NULL;
    s->temperature_mc = 25000;
    s->pin_low = true;
    mclw_regmap_init(&s->map, 1, 0, 0xff);
    power_on(s);
    mclw_regmap_set_on_write(&s->map, ds3231_on_write, s);
    drive_pin(s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void ds3231_deinit(struct mclw_instance *inst) {
    struct ds3231_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    if (s->int_pin != NULL) {
        mclw_hal_gpio_release(s->int_pin);
    }
    s->i2c = NULL;
    s->int_pin = NULL;
}

// The part runs on its battery across a host reset, so reset() keeps the
// clock; reset(true) is a first power-up.
static int ds3231_reset(struct mclw_instance *inst, bool factory) {
    struct ds3231_state *s = inst->state;

    if (factory) {
        uint32_t critical = mclw_hal_critical_enter();
        power_on(s);
        mclw_hal_critical_exit(critical);
    }
    return 0;
}

static void ds3231_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct ds3231_state *s = inst->state;
    bool redrive = false;
    uint32_t critical;

    if (s->chain_reset || s->next_second_us == 0) {
        s->chain_reset = false;
        s->next_second_us = now_us + SECOND_US;
        s->sqw_high = false;
        redrive = true;
    }
    if (s->convert) {
        critical = mclw_hal_critical_enter();
        s->convert = false;
        put_temperature(s);
        put(s, REG_CONTROL, get(s, REG_CONTROL) & (uint8_t)~CONTROL_CONV, MCLW_REG_WRITABLE);
        mclw_hal_critical_exit(critical);
    }
    if (!s->sqw_high && now_us + HALF_SECOND_US >= s->next_second_us) {
        s->sqw_high = true;
        redrive = true;
    }
    if (now_us >= s->next_second_us) {
        uint8_t flags;

        critical = mclw_hal_critical_enter();
        flags = tick(s);
        s->status = (uint8_t)(s->status | flags);
        put(s, REG_STATUS, s->status, MCLW_REG_WRITABLE);
        mclw_hal_critical_exit(critical);
        if ((flags & STATUS_A1F) != 0) {
            s->alarms[0]++;
        }
        if ((flags & STATUS_A2F) != 0) {
            s->alarms[1]++;
        }
        s->next_second_us += SECOND_US;
        if (s->next_second_us <= now_us) {
            s->next_second_us = now_us + SECOND_US;
        }
        s->sqw_high = false;
        redrive = true;
    }
    if (redrive) {
        critical = mclw_hal_critical_enter();
        drive_pin(s);
        mclw_hal_critical_exit(critical);
    }
}

static int ds3231_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct ds3231_state *s = inst->state;

    switch (property) {
        case PROP_TEMPERATURE:
            out->v[0] = s->temperature_mc;
            return 0;
        case PROP_ALARMS:
            out->v[0] = s->alarms[0];
            out->v[1] = s->alarms[1];
            return 0;
        default:
            return -EINVAL;
    }
}

static int ds3231_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct ds3231_state *s = inst->state;

    if (property != PROP_TEMPERATURE) {
        return -EINVAL;
    }
    s->temperature_mc = (int32_t)in->v[0];
    uint32_t critical = mclw_hal_critical_enter();
    put_temperature(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static const struct mclw_model_ops ds3231_ops = {
    .init = ds3231_init,
    .deinit = ds3231_deinit,
    .reset = ds3231_reset,
    .get = ds3231_get,
    .set = ds3231_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = ds3231_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_ds3231 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "ds3231",
    .family = "ds3231",
    .config = NULL,
    .properties = ds3231_properties,
    .property_count = sizeof(ds3231_properties) / sizeof(ds3231_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = ds3231_bindings,
    .binding_count = sizeof(ds3231_bindings) / sizeof(ds3231_bindings[0]),
    .state_size = sizeof(struct ds3231_state),
    .ops = &ds3231_ops,
};
