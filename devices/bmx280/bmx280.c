// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "bmx280.h"

// The order of the descriptor table; the BMP280 uses all but the last.
enum {
    PROP_PRESSURE,
    PROP_TEMPERATURE,
    PROP_POWER_MODE,
    PROP_SPI_MODE,
    PROP_HUMIDITY,
};

enum {
    BINDING_I2C,
    BINDING_SPI,
};

struct bmx280_config {
    uint8_t chip_id;
    bool humidity;
};

struct bmx280_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    struct mclw_spi_target *spi;
    const struct bmx280_config *config;
    bool humidity;
    // 0xff on I2C; 0x7f on SPI, where the part sees seven address bits.
    uint8_t address_mask;
    int32_t pressure_pa;
    int32_t temperature_mc;
    int32_t humidity_mrh;
    uint32_t adc_t;
    uint32_t adc_p;
    uint32_t adc_h;
    // A ctrl_meas write started a measurement; time_advanced completes it.
    volatile bool measuring;
    // ctrl_hum as it stood at the last ctrl_meas write (DS002 5.4.3).
    volatile uint8_t osrs_h;
};

// SPI framing from DS001 5.3 / DS002 6.3: bit 7 of the control byte set for
// a read, seven address bits, read data straight after the control byte
// from auto-incremented addresses.
static const struct mclw_regmap_framing bmx280_framing = {
    .read_bit_mask = 0x80,
    .address_mask = 0x7f,
    .read_dummy_bytes = 0,
    .auto_increment = true,
};

static const struct mclw_property_desc bmx280_properties[] = {
    {
        .name = "pressure",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "Pa",
        .min = BMX280_PRESSURE_MIN_PA,
        .max = BMX280_PRESSURE_MAX_PA,
    },
    {
        .name = "temperature",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mC",
        .min = BMX280_TEMPERATURE_MIN_MC,
        .max = BMX280_TEMPERATURE_MAX_MC,
    },
    {
        .name = "power_mode",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = BMX280_MODE_MASK,
    },
    MCLW_PROPERTY_SPI_MODE,
    {
        .name = "humidity",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "m%RH",
        .min = BME280_HUMIDITY_MIN_MRH,
        .max = BME280_HUMIDITY_MAX_MRH,
    },
};

// One of the two buses must be bound. The part picks its mode from the clock
// level at chip select (DS001 5.3), so both SPI modes it knows are served.
static const struct mclw_binding_desc bmx280_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = false,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
    {
        .name = "spi",
        .kind = MCLW_BINDING_SPI_TARGET,
        .required = false,
        .usb = NULL,
        .spi = {
            .modes = MCLW_SPI_MODE_BIT(0) | MCLW_SPI_MODE_BIT(3),
            .default_mode = 0,
            .msb_first = true,
        },
        .uart = { 0 },
    },
};

static inline uint8_t MCLW_RAM_FUNC(at)(const struct bmx280_state *state, uint8_t reg) {
    return (uint8_t)(reg & state->address_mask);
}

static void MCLW_RAM_FUNC(set_reg)(struct bmx280_state *state, uint8_t reg, uint8_t value,
    uint8_t flags) {
    mclw_regmap_set(&state->map, 0, at(state, reg), value, flags);
}

static uint8_t MCLW_RAM_FUNC(get_reg)(const struct bmx280_state *state, uint8_t reg) {
    return mclw_regmap_get(&state->map, 0, at(state, reg));
}

// A 20-bit word in msb, lsb, xlsb<7:4> (DS001 tables 24, 25).
static void MCLW_RAM_FUNC(set_word20)(struct bmx280_state *state, uint8_t reg, uint32_t word) {
    set_reg(state, reg, (uint8_t)(word >> 12), 0);
    set_reg(state, (uint8_t)(reg + 1u), (uint8_t)(word >> 4), 0);
    set_reg(state, (uint8_t)(reg + 2u), (uint8_t)(word << 4), 0);
}

static void MCLW_RAM_FUNC(set_word16)(struct bmx280_state *state, uint8_t reg, uint32_t word) {
    set_reg(state, reg, (uint8_t)(word >> 8), 0);
    set_reg(state, (uint8_t)(reg + 1u), (uint8_t)word, 0);
}

// Power-on reset, also what writing 0xB6 to reset does (DS001 4.3.2): the
// control registers and the data registers back to their reset states
// (DS001 table 18, DS002 table 18).
static void MCLW_RAM_FUNC(power_on)(struct bmx280_state *state) {
    set_reg(state, BMX280_REG_RESET, 0, MCLW_REG_WRITABLE);
    set_reg(state, BMX280_REG_STATUS, 0, 0);
    set_reg(state, BMX280_REG_CTRL_MEAS, 0, MCLW_REG_WRITABLE);
    set_reg(state, BMX280_REG_CONFIG, 0, MCLW_REG_WRITABLE);
    set_word20(state, BMX280_REG_PRESS_MSB, BMX280_SKIPPED_20);
    set_word20(state, BMX280_REG_TEMP_MSB, BMX280_SKIPPED_20);
    if (state->humidity) {
        set_reg(state, BMX280_REG_CTRL_HUM, 0, MCLW_REG_WRITABLE);
        set_word16(state, BMX280_REG_HUM_MSB, BMX280_SKIPPED_16);
    }
    state->measuring = false;
    state->osrs_h = 0;
}

static void build_map(struct bmx280_state *state) {
    const struct bmx280_calib *c = &mclw_bmx280_calib;
    struct mclw_regmap *map = &state->map;
    const uint16_t words[12] = {
        c->t1, (uint16_t)c->t2, (uint16_t)c->t3, c->p1, (uint16_t)c->p2, (uint16_t)c->p3,
        (uint16_t)c->p4, (uint16_t)c->p5, (uint16_t)c->p6, (uint16_t)c->p7, (uint16_t)c->p8,
        (uint16_t)c->p9,
    };

    mclw_regmap_init(map, 1, 0, 0x00);
    for (uint8_t i = 0; i < 12; i++) {
        mclw_regmap_set_le16(map, 0, at(state, (uint8_t)(BMX280_REG_CALIB_00 + 2u * i)), words[i]);
    }
    set_reg(state, BMX280_REG_ID, state->config->chip_id, 0);
    if (state->humidity) {
        // DS002 table 16: H4 is 0xE4 / 0xE5<3:0>, H5 is 0xE5<7:4> / 0xE6.
        set_reg(state, BMX280_REG_CALIB_25, c->h1, 0);
        mclw_regmap_set_le16(map, 0, at(state, BMX280_REG_CALIB_26), (uint16_t)c->h2);
        set_reg(state, 0xe3, c->h3, 0);
        set_reg(state, 0xe4, (uint8_t)((uint16_t)c->h4 >> 4), 0);
        set_reg(state, 0xe5, (uint8_t)(((uint16_t)c->h4 & 0x0fu) | (((uint16_t)c->h5 & 0x0fu) << 4)), 0);
        set_reg(state, 0xe6, (uint8_t)((uint16_t)c->h5 >> 4), 0);
        set_reg(state, 0xe7, (uint8_t)c->h6, 0);
    }
    power_on(state);
}

// Interrupt context, after the engine committed a bus write, so it and
// everything it calls run from RAM and touch only the image and the flags.
static void MCLW_RAM_FUNC(on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg,
    uint8_t value, void *ctx) {
    struct bmx280_state *state = ctx;
    (void)map;
    (void)bank;

    if (reg == at(state, BMX280_REG_RESET)) {
        if (value == BMX280_RESET_WORD) {
            power_on(state);
        }
        set_reg(state, BMX280_REG_RESET, 0, MCLW_REG_WRITABLE);
    } else if (reg == at(state, BMX280_REG_CTRL_MEAS)) {
        if (state->humidity) {
            state->osrs_h = get_reg(state, BMX280_REG_CTRL_HUM) & BMX280_OSRS_MASK;
        }
        if ((value & BMX280_MODE_MASK) != BMX280_MODE_SLEEP) {
            set_reg(state, BMX280_REG_STATUS, BMX280_STATUS_MEASURING, 0);
            state->measuring = true;
        }
    } else if (reg == at(state, BMX280_REG_CONFIG)) {
        set_reg(state, BMX280_REG_CONFIG, value & BMX280_CONFIG_WRITABLE, MCLW_REG_WRITABLE);
    } else if (state->humidity && reg == at(state, BMX280_REG_CTRL_HUM)) {
        set_reg(state, BMX280_REG_CTRL_HUM, value & BMX280_CTRL_HUM_WRITABLE, MCLW_REG_WRITABLE);
    }
}

// The raw words a driver's compensation turns back into the properties.
static void invert(struct bmx280_state *state) {
    int32_t t_fine;

    state->adc_t = bmx280_raw_temperature(&mclw_bmx280_calib,
        bmx280_centi_c(state->temperature_mc), &t_fine);
    state->adc_p = bmx280_raw_pressure(&mclw_bmx280_calib, t_fine, state->pressure_pa);
    if (state->humidity) {
        state->adc_h = bmx280_raw_humidity(&mclw_bmx280_calib, t_fine,
            bmx280_humidity_q10(state->humidity_mrh));
    }
}

// The measurement a ctrl_meas write started, finished on the next clock
// tick: skipped channels read their skip value (DS001 tables 4, 5; DS002
// table 20), and forced mode falls back to sleep (DS001 3.6.2).
static void finish_measurement(struct bmx280_state *state) {
    uint32_t critical = mclw_hal_critical_enter();
    uint8_t ctrl = get_reg(state, BMX280_REG_CTRL_MEAS);
    uint8_t mode = ctrl & BMX280_MODE_MASK;

    if (!state->measuring) {
        mclw_hal_critical_exit(critical);
        return;
    }
    state->measuring = false;
    set_word20(state, BMX280_REG_TEMP_MSB,
        ((ctrl >> BMX280_OSRS_T_POS) & BMX280_OSRS_MASK) != 0 ? state->adc_t : BMX280_SKIPPED_20);
    set_word20(state, BMX280_REG_PRESS_MSB,
        ((ctrl >> BMX280_OSRS_P_POS) & BMX280_OSRS_MASK) != 0 ? state->adc_p : BMX280_SKIPPED_20);
    if (state->humidity) {
        set_word16(state, BMX280_REG_HUM_MSB, state->osrs_h != 0 ? state->adc_h : BMX280_SKIPPED_16);
    }
    set_reg(state, BMX280_REG_STATUS, 0, 0);
    if (mode != BMX280_MODE_SLEEP && mode != BMX280_MODE_NORMAL) {
        set_reg(state, BMX280_REG_CTRL_MEAS, (uint8_t)(ctrl & ~BMX280_MODE_MASK), MCLW_REG_WRITABLE);
    }
    mclw_hal_critical_exit(critical);
    mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_STATE, at(state, BMX280_REG_PRESS_MSB),
        state->humidity ? 8 : 6, 0);
}

static void disarm(struct bmx280_state *state) {
    if (state->i2c != NULL) {
        mclw_hal_i2c_target_disarm(state->i2c);
    }
    if (state->spi != NULL) {
        mclw_hal_spi_target_disarm(state->spi);
    }
}

static int arm(struct bmx280_state *state) {
    if (state->i2c != NULL) {
        return mclw_hal_i2c_target_arm(state->i2c, &state->map);
    }
    return mclw_hal_spi_target_arm_regmap(state->spi, &state->map, &bmx280_framing);
}

static void defaults(struct bmx280_state *state) {
    state->pressure_pa = BMX280_PRESSURE_DEFAULT_PA;
    state->temperature_mc = BMX280_TEMPERATURE_DEFAULT_MC;
    state->humidity_mrh = BME280_HUMIDITY_DEFAULT_MRH;
    invert(state);
}

static int power_up(struct bmx280_state *state) {
    disarm(state);
    build_map(state);
    mclw_regmap_set_on_write(&state->map, on_write, state);
    return arm(state);
}

static int bmx280_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct bmx280_state *state = inst->state;

    if (bindings[BINDING_I2C].present == bindings[BINDING_SPI].present) {
        return -EINVAL;
    }
    state->i2c = bindings[BINDING_I2C].present ? bindings[BINDING_I2C].i2c : NULL;
    state->spi = bindings[BINDING_SPI].present ? bindings[BINDING_SPI].spi : NULL;
    state->address_mask = state->spi != NULL ? 0x7f : 0xff;
    state->config = inst->model->config;
    state->humidity = state->config->humidity;
    defaults(state);
    return power_up(state);
}

static void bmx280_deinit(struct mclw_instance *inst) {
    struct bmx280_state *state = inst->state;

    disarm(state);
    state->i2c = NULL;
    state->spi = NULL;
}

static int bmx280_reset(struct mclw_instance *inst, bool factory) {
    struct bmx280_state *state = inst->state;

    (void)factory;
    defaults(state);
    return power_up(state);
}

static int bmx280_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct bmx280_state *state = inst->state;

    switch (property) {
        case PROP_PRESSURE:
            out->v[0] = state->pressure_pa;
            return 0;
        case PROP_TEMPERATURE:
            out->v[0] = state->temperature_mc;
            return 0;
        case PROP_HUMIDITY:
            out->v[0] = state->humidity_mrh;
            return 0;
        case PROP_POWER_MODE:
            out->v[0] = get_reg(state, BMX280_REG_CTRL_MEAS) & BMX280_MODE_MASK;
            return 0;
        case PROP_SPI_MODE:
            return mclw_spi_mode_get(inst, BINDING_SPI, out);
        default:
            return -EINVAL;
    }
}

// A new value shows in the data registers at the next measurement: on the
// next tick in normal mode, at the next forced one otherwise.
static int bmx280_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct bmx280_state *state = inst->state;
    uint32_t critical;

    switch (property) {
        case PROP_PRESSURE:
            state->pressure_pa = (int32_t)in->v[0];
            break;
        case PROP_TEMPERATURE:
            state->temperature_mc = (int32_t)in->v[0];
            break;
        case PROP_HUMIDITY:
            state->humidity_mrh = (int32_t)in->v[0];
            break;
        case PROP_SPI_MODE:
            return mclw_spi_mode_set(inst, BINDING_SPI, in);
        default:
            return -EINVAL;
    }
    invert(state);
    critical = mclw_hal_critical_enter();
    if ((get_reg(state, BMX280_REG_CTRL_MEAS) & BMX280_MODE_MASK) == BMX280_MODE_NORMAL) {
        state->measuring = true;
    }
    mclw_hal_critical_exit(critical);
    return 0;
}

static void bmx280_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    (void)now_us;
    finish_measurement(inst->state);
}

static const struct mclw_model_ops bmx280_ops = {
    .init = bmx280_init,
    .deinit = bmx280_deinit,
    .reset = bmx280_reset,
    .get = bmx280_get,
    .set = bmx280_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = bmx280_time_advanced,
    .usb_rx = NULL,
};

#define BMX280_MODEL(symbol, name, chip, has_humidity, property_total) \
    static const struct bmx280_config symbol##_config = { \
        .chip_id = (chip), \
        .humidity = (has_humidity), \
    }; \
    const struct mclw_model symbol = { \
        .abi_version = MCLW_ABI_VERSION, \
        .id = (name), \
        .family = "bmx280", \
        .config = &symbol##_config, \
        .properties = bmx280_properties, \
        .property_count = (property_total), \
        .regions = NULL, \
        .region_count = 0, \
        .faults = NULL, \
        .fault_count = 0, \
        .bindings = bmx280_bindings, \
        .binding_count = sizeof(bmx280_bindings) / sizeof(bmx280_bindings[0]), \
        .state_size = sizeof(struct bmx280_state), \
        .ops = &bmx280_ops, \
    }

BMX280_MODEL(mclw_model_bmp280, "bmp280", BMP280_CHIP_ID, false, PROP_HUMIDITY);
BMX280_MODEL(mclw_model_bme280, "bme280", BME280_CHIP_ID, true, PROP_HUMIDITY + 1);
