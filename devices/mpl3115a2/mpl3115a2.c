// SPDX-License-Identifier: Apache-2.0 OR MIT
// NXP MPL3115A2 pressure/altitude sensor on I2C, with its INT1 and INT2
// lines. Register map, data formats, FIFO, interrupts and timing from the
// data sheet, Rev. 8.1, 17 May 2024 (sections cited where used).

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

#include "mpl3115a2.h"

// Table 10.
#define REG_STATUS 0x00
#define REG_OUT_P_MSB 0x01
#define REG_OUT_T_LSB 0x05
#define REG_DR_STATUS 0x06
#define REG_WHO_AM_I 0x0c
#define REG_F_STATUS 0x0d
#define REG_F_DATA 0x0e
#define REG_F_SETUP 0x0f
#define REG_SYSMOD 0x11
#define REG_INT_SOURCE 0x12
#define REG_PT_DATA_CFG 0x13
#define REG_BAR_IN_MSB 0x14
#define REG_BAR_IN_LSB 0x15
#define REG_CTRL_REG1 0x26
#define REG_CTRL_REG2 0x27
#define REG_CTRL_REG3 0x28
#define REG_CTRL_REG4 0x29
#define REG_CTRL_REG5 0x2a
#define REG_OFF_P 0x2b
#define REG_OFF_T 0x2c
#define REG_OFF_H 0x2d

#define WHO_AM_I_MPL3115A2 0xc4
#define BAR_IN_RESET 0xc5e7

// Table 44: ALT, OS[2:0], RST, OST, SBYB.
#define CTRL_REG1_ALT 0x80u
#define CTRL_REG1_OS_SHIFT 3
#define CTRL_REG1_RST 0x04u
#define CTRL_REG1_OST 0x02u
#define CTRL_REG1_SBYB 0x01u
// Table 47: ST[3:0].
#define CTRL_REG2_ST 0x0fu
// Table 49: IPOL1, IPOL2.
#define CTRL_REG3_IPOL1 0x20u
#define CTRL_REG3_IPOL2 0x02u
// Tables 26, 21: F_MODE[1:0], F_WMRK[5:0]; F_OVF, F_WMRK_FLAG, F_CNT[5:0].
#define F_SETUP_MODE_SHIFT 6
#define F_SETUP_WMRK 0x3fu
#define F_STATUS_OVF 0x80u
#define F_STATUS_WMRK 0x40u
// Table 31.
#define SRC_DRDY 0x80u
#define SRC_FIFO 0x40u
// Table 14: PTDR, PDR, TDR.
#define DR_STATUS_READY 0x0eu

#define FIFO_SAMPLES 32
#define SAMPLE_BYTES 5

// Behind the registers: the alias F_STATUS reads at 00h in FIFO mode, then
// the FIFO's 160 bytes, so a burst from 00h or 01h walks the FIFO (Table
// 12, 14.8.2), then four zeros for 02h..05h (Table 12).
#define IMAGE_F_STATUS_ALIAS 0x3f
#define IMAGE_FIFO 0x40
#define IMAGE_ZEROS (IMAGE_FIFO + FIFO_SAMPLES * SAMPLE_BYTES)

enum {
    PROP_PRESSURE,
    PROP_ALTITUDE,
    PROP_TEMPERATURE,
};

enum {
    BINDING_I2C,
    BINDING_INT1,
    BINDING_INT2,
};

struct mpl3115a2_state {
    struct mclw_regmap map;
    uint8_t pointers[MCLW_REGMAP_BANK_SIZE];
    struct mclw_i2c_target *i2c;
    struct mclw_gpio *int1;
    struct mclw_gpio *int2;
    int32_t pressure_pa;
    int32_t altitude_mm;
    int32_t temperature_mc;
    uint8_t count;
    // F_STATUS as last published; a read clears the image's copies.
    uint8_t f_status;
    // Set from on_write, acted on by the worker.
    bool start;
    bool one_shot;
    bool active;
    uint64_t next_sample_us;
};

static const struct mclw_property_desc mpl3115a2_properties[] = {
    {
        .name = "pressure",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "Pa",
        .min = 20000,
        .max = 110000,
    },
    {
        .name = "altitude",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mm",
        .min = -698000,
        .max = 11775000,
    },
    {
        .name = "temperature",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mC",
        .min = -40000,
        .max = 85000,
    },
};

static const struct mclw_binding_desc mpl3115a2_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
    {
        .name = "int1",
        .kind = MCLW_BINDING_GPIO_OUT,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
    {
        .name = "int2",
        .kind = MCLW_BINDING_GPIO_OUT,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static uint8_t MCLW_RAM_FUNC(reg)(const struct mpl3115a2_state *s, uint8_t r) {
    return mclw_regmap_get(&s->map, 0, r);
}

static void MCLW_RAM_FUNC(route_pointers)(struct mpl3115a2_state *s) {
    bool fifo = (reg(s, REG_F_SETUP) >> F_SETUP_MODE_SHIFT) != 0;

    for (unsigned p = 0; p < MCLW_REGMAP_BANK_SIZE; p++) {
        s->pointers[p] = (uint8_t)p;
    }
    s->pointers[REG_F_DATA] = IMAGE_FIFO;
    if (fifo) {
        s->pointers[REG_STATUS] = IMAGE_F_STATUS_ALIAS;
        s->pointers[REG_OUT_P_MSB] = IMAGE_FIFO;
        for (uint8_t p = REG_OUT_P_MSB + 1; p <= REG_OUT_T_LSB; p++) {
            s->pointers[p] = (uint8_t)(IMAGE_ZEROS + p - (REG_OUT_P_MSB + 1));
        }
    }
}

static void MCLW_RAM_FUNC(put_f_status)(struct mpl3115a2_state *s, uint8_t value) {
    s->f_status = value;
    mclw_regmap_set(&s->map, 0, REG_F_STATUS, value, MCLW_REG_CLEAR_ON_READ);
    mclw_regmap_set(&s->map, 0, IMAGE_F_STATUS_ALIAS, value, MCLW_REG_CLEAR_ON_READ);
}

static void MCLW_RAM_FUNC(flush)(struct mpl3115a2_state *s) {
    s->count = 0;
    put_f_status(s, 0);
}

// Table 10: what the bus may write. The rest, and everything behind the
// registers, is read-only.
static bool MCLW_RAM_FUNC(writable)(unsigned r) {
    return r == REG_F_SETUP || (r >= REG_PT_DATA_CFG && r <= REG_OFF_H);
}

static void MCLW_RAM_FUNC(reset_registers)(struct mpl3115a2_state *s) {
    for (unsigned r = 0; r < MCLW_REGMAP_BANK_SIZE; r++) {
        mclw_regmap_set(&s->map, 0, (uint8_t)r, 0, writable(r) ? MCLW_REG_WRITABLE : 0);
    }
    mclw_regmap_set(&s->map, 0, REG_WHO_AM_I, WHO_AM_I_MPL3115A2, 0);
    mclw_regmap_set(&s->map, 0, REG_BAR_IN_MSB, (uint8_t)(BAR_IN_RESET >> 8), MCLW_REG_WRITABLE);
    mclw_regmap_set(&s->map, 0, REG_BAR_IN_LSB, (uint8_t)BAR_IN_RESET, MCLW_REG_WRITABLE);
    s->active = false;
    s->start = false;
    s->one_shot = false;
    flush(s);
    route_pointers(s);
}

static void MCLW_RAM_FUNC(mpl3115a2_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t r,
    uint8_t value, void *ctx) {
    struct mpl3115a2_state *s = ctx;

    (void)map;
    (void)bank;
    if (r == REG_CTRL_REG1) {
        // RST reboots into STANDBY and reads back 0 (Table 45).
        if (value & CTRL_REG1_RST) {
            reset_registers(s);
            return;
        }
        bool active = (value & CTRL_REG1_SBYB) != 0;
        s->start = s->start || (active && !s->active);
        s->one_shot = !active && (value & CTRL_REG1_OST) != 0;
        s->active = active;
        mclw_regmap_set(&s->map, 0, REG_SYSMOD, active ? 1 : 0, 0);
    } else if (r == REG_F_SETUP) {
        // Disabling the FIFO flushes it (Table 27).
        if ((value >> F_SETUP_MODE_SHIFT) == 0) {
            flush(s);
        }
        route_pointers(s);
    }
}

// 14.3, 14.4: altitude Q16.4 m or pressure Q18.2 Pa in 20 bits over MSB,
// CSB and LSB[7:4]; temperature Q8.4 C in 12 bits over MSB and LSB[7:4].
// OFF_H is 1 m, OFF_P 4 Pa and OFF_T 0.0625 C per LSB (14.23).
static void MCLW_RAM_FUNC(encode)(const struct mpl3115a2_state *s, uint8_t out[SAMPLE_BYTES]) {
    int64_t p;
    int64_t t = ((int64_t)s->temperature_mc * 16 + (s->temperature_mc >= 0 ? 500 : -500)) / 1000
        + (int8_t)reg(s, REG_OFF_T);

    if (reg(s, REG_CTRL_REG1) & CTRL_REG1_ALT) {
        p = ((int64_t)s->altitude_mm * 16 + (s->altitude_mm >= 0 ? 500 : -500)) / 1000
            + (int64_t)(int8_t)reg(s, REG_OFF_H) * 16;
        p = p < -524288 ? -524288 : (p > 524287 ? 524287 : p);
    } else {
        p = (int64_t)s->pressure_pa * 4 + (int64_t)(int8_t)reg(s, REG_OFF_P) * 16;
        p = p < 0 ? 0 : (p > 0xfffff ? 0xfffff : p);
    }
    t = t < -2048 ? -2048 : (t > 2047 ? 2047 : t);
    uint32_t p20 = (uint32_t)p & 0xfffffu;
    uint32_t t12 = (uint32_t)t & 0xfffu;
    out[0] = (uint8_t)(p20 >> 12);
    out[1] = (uint8_t)(p20 >> 4);
    out[2] = (uint8_t)((p20 & 0xfu) << 4);
    out[3] = (uint8_t)(t12 >> 4);
    out[4] = (uint8_t)((t12 & 0xfu) << 4);
}

// One acquisition: into OUT_P/OUT_T with FIFO disabled, else into the FIFO,
// which overflows at 32 samples, dropping the new sample in full-stop mode
// and the oldest in circular mode (Table 27).
static void MCLW_RAM_FUNC(acquire)(struct mpl3115a2_state *s) {
    uint8_t sample[SAMPLE_BYTES];
    uint8_t fifo_mode = reg(s, REG_F_SETUP) >> F_SETUP_MODE_SHIFT;
    uint8_t watermark = reg(s, REG_F_SETUP) & F_SETUP_WMRK;
    uint8_t source = reg(s, REG_INT_SOURCE);

    encode(s, sample);
    if (fifo_mode == 0) {
        for (uint8_t i = 0; i < SAMPLE_BYTES; i++) {
            mclw_regmap_set(&s->map, 0, (uint8_t)(REG_OUT_P_MSB + i), sample[i], 0);
        }
        mclw_regmap_set(&s->map, 0, REG_DR_STATUS, DR_STATUS_READY, 0);
        mclw_regmap_set(&s->map, 0, REG_STATUS, DR_STATUS_READY, 0);
        source |= SRC_DRDY;
    } else {
        bool overflowed = (s->f_status & F_STATUS_OVF) != 0;
        if (s->count == FIFO_SAMPLES && fifo_mode == 1) {
            for (uint8_t i = 0; i < (FIFO_SAMPLES - 1) * SAMPLE_BYTES; i++) {
                mclw_regmap_set(&s->map, 0, (uint8_t)(IMAGE_FIFO + i),
                    reg(s, (uint8_t)(IMAGE_FIFO + SAMPLE_BYTES + i)), 0);
            }
            s->count--;
        }
        if (s->count < FIFO_SAMPLES) {
            for (uint8_t i = 0; i < SAMPLE_BYTES; i++) {
                mclw_regmap_set(&s->map, 0, (uint8_t)(IMAGE_FIFO + s->count * SAMPLE_BYTES + i), sample[i], 0);
            }
            s->count++;
        }
        bool full = s->count == FIFO_SAMPLES;
        bool at_watermark = watermark != 0 && s->count == watermark;
        if ((full && !overflowed) || at_watermark) {
            source |= SRC_FIFO;
        }
        put_f_status(s, (uint8_t)((full ? F_STATUS_OVF : 0) | (s->count >= watermark && watermark != 0 ? F_STATUS_WMRK : 0)
            | s->count));
    }
    mclw_regmap_set(&s->map, 0, REG_INT_SOURCE, source, 0);
}

// A read of F_STATUS clears SRC_FIFO (Table 32). The FIFO is taken to have
// been drained with it, as a driver does before or after that read.
static void MCLW_RAM_FUNC(check_f_status_read)(struct mpl3115a2_state *s) {
    if (s->f_status != 0
        && (reg(s, REG_F_STATUS) != s->f_status || reg(s, IMAGE_F_STATUS_ALIAS) != s->f_status)) {
        mclw_regmap_set(&s->map, 0, REG_INT_SOURCE, (uint8_t)(reg(s, REG_INT_SOURCE) & ~SRC_FIFO), 0);
        flush(s);
    }
}

// 14.22.2: one sample every 2^ST s, no faster than the oversampling allows
// (Table 46).
static uint64_t period_us(const struct mpl3115a2_state *s) {
    static const uint16_t os_min_ms[8] = { 6, 10, 18, 34, 66, 130, 258, 512 };
    uint64_t step = 1000000ull << (reg(s, REG_CTRL_REG2) & CTRL_REG2_ST);
    uint64_t floor = (uint64_t)os_min_ms[(reg(s, REG_CTRL_REG1) >> CTRL_REG1_OS_SHIFT) & 7u] * 1000u;

    return step > floor ? step : floor;
}

// 14.22.3..5: an enabled source drives INT1 if CTRL_REG5 routes it there,
// INT2 otherwise; IPOL picks the active level. Both pins are driven
// push-pull whatever PP_OD says.
static void drive_pins(struct mpl3115a2_state *s) {
    uint8_t enabled = reg(s, REG_INT_SOURCE) & reg(s, REG_CTRL_REG4);
    uint8_t route = reg(s, REG_CTRL_REG5);
    uint8_t polarity = reg(s, REG_CTRL_REG3);

    if (s->int1 != NULL) {
        bool asserted = (enabled & route) != 0;
        mclw_hal_gpio_write(s->int1, asserted == ((polarity & CTRL_REG3_IPOL1) != 0));
    }
    if (s->int2 != NULL) {
        bool asserted = (enabled & (uint8_t)~route) != 0;
        mclw_hal_gpio_write(s->int2, asserted == ((polarity & CTRL_REG3_IPOL2) != 0));
    }
}

static void mpl3115a2_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct mpl3115a2_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();

    check_f_status_read(s);
    if (s->start) {
        // STANDBY to ACTIVE flushes the FIFO and resets the data (Table 27,
        // Table 10 note 2).
        s->start = false;
        flush(s);
        for (uint8_t r = REG_STATUS; r <= REG_DR_STATUS; r++) {
            mclw_regmap_set(&s->map, 0, r, 0, 0);
        }
        s->next_sample_us = now_us + period_us(s);
    }
    if (s->one_shot) {
        s->one_shot = false;
        acquire(s);
        mclw_regmap_set(&s->map, 0, REG_CTRL_REG1, (uint8_t)(reg(s, REG_CTRL_REG1) & ~CTRL_REG1_OST),
            MCLW_REG_WRITABLE);
    }
    if (s->active && now_us >= s->next_sample_us) {
        acquire(s);
        s->next_sample_us += period_us(s);
        if (s->next_sample_us <= now_us) {
            s->next_sample_us = now_us + period_us(s);
        }
    }
    mclw_hal_critical_exit(critical);
    drive_pins(s);
}

static void defaults(struct mpl3115a2_state *s) {
    s->pressure_pa = 101326;
    s->altitude_mm = 0;
    s->temperature_mc = 25000;
}

static int mpl3115a2_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct mpl3115a2_state *s = inst->state;

    s->i2c = bindings[BINDING_I2C].i2c;
    s->int1 = bindings[BINDING_INT1].present ? bindings[BINDING_INT1].gpio : NULL;
    s->int2 = bindings[BINDING_INT2].present ? bindings[BINDING_INT2].gpio : NULL;
    defaults(s);
    mclw_regmap_init(&s->map, 1, 0, 0x00);
    reset_registers(s);
    mclw_regmap_set_pointer_map(&s->map, s->pointers);
    mclw_regmap_set_on_write(&s->map, mpl3115a2_on_write, s);
    drive_pins(s);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void mpl3115a2_deinit(struct mclw_instance *inst) {
    struct mpl3115a2_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
    s->int1 = NULL;
    s->int2 = NULL;
}

static int mpl3115a2_reset(struct mclw_instance *inst, bool factory) {
    struct mpl3115a2_state *s = inst->state;

    (void)factory;
    uint32_t critical = mclw_hal_critical_enter();
    defaults(s);
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    drive_pins(s);
    return 0;
}

static int mpl3115a2_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct mpl3115a2_state *s = inst->state;

    switch (property) {
        case PROP_PRESSURE:
            out->v[0] = s->pressure_pa;
            return 0;
        case PROP_ALTITUDE:
            out->v[0] = s->altitude_mm;
            return 0;
        case PROP_TEMPERATURE:
            out->v[0] = s->temperature_mc;
            return 0;
        default:
            return -EINVAL;
    }
}

// The next acquisition reports the new value.
static int mpl3115a2_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct mpl3115a2_state *s = inst->state;
    int32_t v = (int32_t)in->v[0];

    switch (property) {
        case PROP_PRESSURE:
            s->pressure_pa = v;
            break;
        case PROP_ALTITUDE:
            s->altitude_mm = v;
            break;
        case PROP_TEMPERATURE:
            s->temperature_mc = v;
            break;
        default:
            return -EINVAL;
    }
    mclw_trace(inst, MCLW_TRACE_STATE, (uint32_t)property, 1, 0);
    return 0;
}

static const struct mclw_model_ops mpl3115a2_ops = {
    .init = mpl3115a2_init,
    .deinit = mpl3115a2_deinit,
    .reset = mpl3115a2_reset,
    .get = mpl3115a2_get,
    .set = mpl3115a2_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = mpl3115a2_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_mpl3115a2 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "mpl3115a2",
    .family = "mpl3115a2",
    .config = NULL,
    .properties = mpl3115a2_properties,
    .property_count = sizeof(mpl3115a2_properties) / sizeof(mpl3115a2_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = mpl3115a2_bindings,
    .binding_count = sizeof(mpl3115a2_bindings) / sizeof(mpl3115a2_bindings[0]),
    .state_size = sizeof(struct mpl3115a2_state),
    .ops = &mpl3115a2_ops,
};
