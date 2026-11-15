// SPDX-License-Identifier: Apache-2.0 OR MIT

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

#include "pcf8523.h"

enum {
    PROP_TIME,
    PROP_STOP,
    PROP_OS,
    PROP_AF,
};

enum {
    BINDING_I2C,
};

struct pcf8523_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    // Control_2's flags, which a write can only clear.
    uint8_t flags;
    // Bumped by every bus write to the time registers, so a count done
    // outside the critical section never overwrites what the DUT set.
    volatile uint32_t time_writes;
    uint64_t next_tick_us;
    bool stopped;
};

// Attach is a power-on: the time counters are undefined there (table 10),
// and the model starts them at 2000-01-01, a Saturday, 00:00:00.
static const struct pcf8523_time power_on_time = {
    .year = 0,
    .month = 1,
    .day = 1,
    .weekday = 6,
    .hour = 0,
    .minute = 0,
    .second = 0,
};

static const struct mclw_property_desc pcf8523_properties[] = {
    {
        // year (0..99), month, day, weekday (0 Sunday), hour (0..23),
        // minute, second.
        .name = "time",
        .type = MCLW_TYPE_I32,
        .count = PCF8523_TIME_REGS,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 0,
        .max = 99,
    },
    {
        .name = "stop",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    {
        .name = "os",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    {
        .name = "af",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
};

static const struct mclw_binding_desc pcf8523_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static uint8_t MCLW_RAM_FUNC(get_reg)(const struct pcf8523_state *state, uint8_t reg) {
    return mclw_regmap_get(&state->map, 0, reg);
}

static void MCLW_RAM_FUNC(set_reg)(struct pcf8523_state *state, uint8_t reg, uint8_t value) {
    mclw_regmap_set(&state->map, 0, reg, value, MCLW_REG_WRITABLE);
}

// The bits each register implements; the rest read 0 (table 6).
static uint8_t MCLW_RAM_FUNC(implemented)(uint8_t reg) {
    switch (reg) {
        case 0x04:
            return 0x7f;
        case 0x05:
        case 0x06:
            return 0x3f;
        case 0x07:
            return 0x07;
        case 0x08:
            return 0x1f;
        case 0x0b:
        case 0x0c:
            return 0xbf;
        case 0x0d:
            return 0x87;
        case PCF8523_REG_TMR_A_FREQ_CTRL:
            return 0x07;
        case PCF8523_REG_TMR_B_FREQ_CTRL:
            return 0x77;
        default:
            return 0xff;
    }
}

// Software reset (8.3, table 10): the control, alarm-enable and timer
// configuration bits to their reset values, OS set, the counters kept.
static void MCLW_RAM_FUNC(software_reset)(struct pcf8523_state *state) {
    set_reg(state, PCF8523_REG_CONTROL_1, 0);
    set_reg(state, PCF8523_REG_CONTROL_2, 0);
    set_reg(state, PCF8523_REG_CONTROL_3, PCF8523_CONTROL_3_RESET);
    set_reg(state, PCF8523_REG_SECONDS, get_reg(state, PCF8523_REG_SECONDS) | PCF8523_SECONDS_OS);
    for (uint8_t reg = PCF8523_REG_MINUTE_ALARM; reg <= PCF8523_REG_WEEKDAY_ALARM; reg++) {
        set_reg(state, reg, get_reg(state, reg) | PCF8523_ALARM_AEN);
    }
    set_reg(state, PCF8523_REG_OFFSET, 0);
    set_reg(state, PCF8523_REG_TMR_CLKOUT_CTRL, 0);
    set_reg(state, PCF8523_REG_TMR_A_FREQ_CTRL, PCF8523_TMR_FREQ_CTRL_RESET);
    set_reg(state, PCF8523_REG_TMR_B_FREQ_CTRL, PCF8523_TMR_FREQ_CTRL_RESET);
    state->flags = 0;
    state->time_writes++;
}

// Interrupt context, once per byte the engine committed; edits only the
// image and the state, from RAM.
static void MCLW_RAM_FUNC(on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx) {
    struct pcf8523_state *state = ctx;
    (void)map;
    (void)bank;

    switch (reg) {
        case PCF8523_REG_CONTROL_1:
            if (value == PCF8523_SOFTWARE_RESET) {
                software_reset(state);
            } else {
                set_reg(state, reg, value & (uint8_t)~PCF8523_CONTROL_1_SR);
            }
            break;
        case PCF8523_REG_CONTROL_2:
            state->flags = (uint8_t)((state->flags & PCF8523_CONTROL_2_WTAF)
                | (state->flags & value & PCF8523_CONTROL_2_FLAGS));
            set_reg(state, reg, (uint8_t)(state->flags | (value & PCF8523_CONTROL_2_ENABLES)));
            break;
        case PCF8523_REG_CONTROL_3:
            set_reg(state, reg, value & PCF8523_CONTROL_3_WRITABLE);
            break;
        default:
            if (reg >= PCF8523_REGISTERS) {
                return;
            }
            set_reg(state, reg, value & implemented(reg));
            if (reg >= PCF8523_REG_SECONDS && reg <= PCF8523_REG_YEARS) {
                state->time_writes++;
            }
            break;
    }
}

static bool twelve_hour(const struct pcf8523_state *state) {
    return (get_reg(state, PCF8523_REG_CONTROL_1) & PCF8523_CONTROL_1_12_24) != 0;
}

static void read_time(const struct pcf8523_state *state, uint8_t regs[PCF8523_TIME_REGS]) {
    for (uint8_t i = 0; i < PCF8523_TIME_REGS; i++) {
        regs[i] = get_reg(state, (uint8_t)(PCF8523_REG_SECONDS + i));
    }
}

static void write_time(struct pcf8523_state *state, const uint8_t regs[PCF8523_TIME_REGS]) {
    for (uint8_t i = 0; i < PCF8523_TIME_REGS; i++) {
        set_reg(state, (uint8_t)(PCF8523_REG_SECONDS + i), regs[i]);
    }
}

static void build_map(struct pcf8523_state *state) {
    uint8_t regs[PCF8523_TIME_REGS];

    mclw_regmap_init(&state->map, 1, 0, 0x00);
    for (uint8_t reg = 0; reg < PCF8523_REGISTERS; reg++) {
        set_reg(state, reg, 0);
    }
    pcf8523_encode(&power_on_time, false, regs);
    write_time(state, regs);
    software_reset(state);
    mclw_regmap_set_on_write(&state->map, on_write, state);
}

static int power_up(struct pcf8523_state *state) {
    mclw_hal_i2c_target_disarm(state->i2c);
    build_map(state);
    state->stopped = false;
    state->next_tick_us = mclw_now_us() + PCF8523_SECOND_US;
    return mclw_hal_i2c_target_arm(state->i2c, &state->map);
}

static int pcf8523_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct pcf8523_state *state = inst->state;

    state->i2c = bindings[BINDING_I2C].i2c;
    return power_up(state);
}

static void pcf8523_deinit(struct mclw_instance *inst) {
    struct pcf8523_state *state = inst->state;

    mclw_hal_i2c_target_disarm(state->i2c);
    state->i2c = NULL;
}

static int pcf8523_reset(struct mclw_instance *inst, bool factory) {
    (void)factory;
    return power_up(inst->state);
}

static int pcf8523_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct pcf8523_state *state = inst->state;
    uint8_t regs[PCF8523_TIME_REGS];
    struct pcf8523_time t;
    uint32_t critical;

    switch (property) {
        case PROP_TIME:
            critical = mclw_hal_critical_enter();
            read_time(state, regs);
            pcf8523_decode(regs, twelve_hour(state), &t);
            mclw_hal_critical_exit(critical);
            out->v[0] = t.year;
            out->v[1] = t.month;
            out->v[2] = t.day;
            out->v[3] = t.weekday;
            out->v[4] = t.hour;
            out->v[5] = t.minute;
            out->v[6] = t.second;
            return 0;
        case PROP_STOP:
            out->v[0] = (get_reg(state, PCF8523_REG_CONTROL_1) & PCF8523_CONTROL_1_STOP) != 0;
            return 0;
        case PROP_OS:
            out->v[0] = (get_reg(state, PCF8523_REG_SECONDS) & PCF8523_SECONDS_OS) != 0;
            return 0;
        case PROP_AF:
            out->v[0] = (get_reg(state, PCF8523_REG_CONTROL_2) & PCF8523_CONTROL_2_AF) != 0;
            return 0;
        default:
            return -EINVAL;
    }
}

// Setting the time from the host restarts the second: the next increment
// comes a whole second later.
static int pcf8523_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct pcf8523_state *state = inst->state;
    struct pcf8523_time t;
    uint8_t regs[PCF8523_TIME_REGS];
    uint32_t critical;

    if (property != PROP_TIME) {
        return -EINVAL;
    }
    t.year = (uint8_t)in->v[0];
    t.month = (uint8_t)in->v[1];
    t.day = (uint8_t)in->v[2];
    t.weekday = (uint8_t)in->v[3];
    t.hour = (uint8_t)in->v[4];
    t.minute = (uint8_t)in->v[5];
    t.second = (uint8_t)in->v[6];
    if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > pcf8523_days_in_month(t.month, t.year)
        || t.weekday > 6 || t.hour > 23 || t.minute > 59 || t.second > 59) {
        return -EINVAL;
    }
    critical = mclw_hal_critical_enter();
    pcf8523_encode(&t, twelve_hour(state), regs);
    regs[0] |= get_reg(state, PCF8523_REG_SECONDS) & PCF8523_SECONDS_OS;
    write_time(state, regs);
    state->time_writes++;
    mclw_hal_critical_exit(critical);
    state->next_tick_us = mclw_now_us() + PCF8523_SECOND_US;
    mclw_trace(inst, MCLW_TRACE_STATE, PCF8523_REG_SECONDS, PCF8523_TIME_REGS, 0);
    return 0;
}

// One 1 Hz tick per second of the simulation clock while STOP is clear. The
// counting is done on a copy outside the critical section (a long
// advance_us can be many minutes of it) and dropped if the DUT wrote the
// time meanwhile.
static void pcf8523_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct pcf8523_state *state = inst->state;
    uint8_t regs[PCF8523_TIME_REGS];
    uint8_t alarm[PCF8523_ALARM_REGS];
    uint64_t ticks;
    uint32_t writes;
    uint32_t critical;
    bool twelve;
    bool flagged;

    if ((get_reg(state, PCF8523_REG_CONTROL_1) & PCF8523_CONTROL_1_STOP) != 0) {
        state->stopped = true;
        return;
    }
    if (state->stopped) {
        state->stopped = false;
        state->next_tick_us = now_us + PCF8523_STOP_RELEASE_US;
        return;
    }
    if (now_us < state->next_tick_us) {
        return;
    }
    ticks = (now_us - state->next_tick_us) / PCF8523_SECOND_US + 1u;
    state->next_tick_us += ticks * PCF8523_SECOND_US;

    critical = mclw_hal_critical_enter();
    read_time(state, regs);
    for (uint8_t i = 0; i < PCF8523_ALARM_REGS; i++) {
        alarm[i] = get_reg(state, (uint8_t)(PCF8523_REG_MINUTE_ALARM + i));
    }
    twelve = twelve_hour(state);
    writes = state->time_writes;
    mclw_hal_critical_exit(critical);

    flagged = pcf8523_advance(regs, twelve, ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks, alarm);

    critical = mclw_hal_critical_enter();
    if (writes == state->time_writes) {
        write_time(state, regs);
        if (flagged) {
            state->flags |= PCF8523_CONTROL_2_AF;
            set_reg(state, PCF8523_REG_CONTROL_2, get_reg(state, PCF8523_REG_CONTROL_2) | PCF8523_CONTROL_2_AF);
        }
    }
    mclw_hal_critical_exit(critical);
    if (flagged) {
        mclw_trace(inst, MCLW_TRACE_STATE, PCF8523_REG_CONTROL_2, 1, PCF8523_CONTROL_2_AF);
    }
}

static const struct mclw_model_ops pcf8523_ops = {
    .init = pcf8523_init,
    .deinit = pcf8523_deinit,
    .reset = pcf8523_reset,
    .get = pcf8523_get,
    .set = pcf8523_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = pcf8523_time_advanced,
    .usb_rx = NULL,
};

const struct mclw_model mclw_model_pcf8523 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "pcf8523",
    .family = "pcf8523",
    .config = NULL,
    .properties = pcf8523_properties,
    .property_count = sizeof(pcf8523_properties) / sizeof(pcf8523_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = pcf8523_bindings,
    .binding_count = sizeof(pcf8523_bindings) / sizeof(pcf8523_bindings[0]),
    .state_size = sizeof(struct pcf8523_state),
    .ops = &pcf8523_ops,
};
