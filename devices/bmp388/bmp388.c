// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"
#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "bmp388.h"

enum {
    PROP_PRESSURE,
    PROP_TEMPERATURE,
    PROP_POWER_MODE,
    PROP_OSR,
    PROP_ODR,
    PROP_SPI_MODE,
};

enum {
    BINDING_I2C,
    BINDING_SPI,
    BINDING_INT,
};

#define BMP388_RAW_MAX 0xffffff

struct bmp388_state {
    struct mclw_regmap map;
    struct mclw_i2c_target *i2c;
    struct mclw_spi_target *spi;
    int32_t pressure_pa;
    int32_t temperature_mc;
    // INT (datasheet 3.7): the pin, when bound; the next conversion in
    // normal mode and its period (0: not converting); when data-ready last
    // asserted.
    struct mclw_gpio *int_pin;
    uint64_t next_us;
    uint32_t period_us;
    uint64_t drdy_at_us;
};

// SPI framing from the datasheet: bit 7 of the command byte set for a read,
// seven address bits, one dummy byte before read data, consecutive addresses.
static const struct mclw_regmap_framing bmp388_framing = {
    .read_bit_mask = 0x80,
    .address_mask = 0x7f,
    .read_dummy_bytes = 1,
    .auto_increment = true,
};

static const struct mclw_property_desc bmp388_properties[] = {
    {
        .name = "pressure",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "Pa",
        .min = BMP388_PRESSURE_MIN_PA,
        .max = BMP388_PRESSURE_MAX_PA,
    },
    {
        .name = "temperature",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mC",
        .min = BMP388_TEMPERATURE_MIN_MC,
        .max = BMP388_TEMPERATURE_MAX_MC,
    },
    {
        .name = "power_mode",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = BMP388_PWR_CTRL_MODE_MASK,
    },
    {
        .name = "osr",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = BMP388_OSR_MASK,
    },
    {
        .name = "odr",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = BMP388_ODR_MASK,
    },
    MCLW_PROPERTY_SPI_MODE,
};

// One of the two buses must be bound; the part has both interfaces and the
// same register map behind them.
static const struct mclw_binding_desc bmp388_bindings[] = {
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
            .default_mode = 3,
            .msb_first = true,
        },
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

static uint32_t clamp_raw(int64_t value) {
    if (value < 0) {
        return 0;
    }
    if (value > BMP388_RAW_MAX) {
        return BMP388_RAW_MAX;
    }
    return (uint32_t)value;
}

uint32_t mclw_bmp388_raw_pressure(int32_t pressure_pa) {
    int64_t millipascal = (int64_t)pressure_pa * 1000;
    int64_t raw = (millipascal * BMP388_PRESSURE_NUMERATOR + BMP388_PRESSURE_DENOMINATOR / 2)
        / BMP388_PRESSURE_DENOMINATOR;

    return clamp_raw(raw);
}

uint32_t mclw_bmp388_raw_temperature(int32_t temperature_mc) {
    int64_t offset = ((int64_t)temperature_mc * BMP388_TEMPERATURE_NUMERATOR)
        / BMP388_TEMPERATURE_DENOMINATOR;

    return clamp_raw((int64_t)BMP388_NVM_PAR_T1 * 256 + offset);
}

static void build_calibration(struct mclw_regmap *map) {
    mclw_regmap_set_le16(map, 0, BMP388_REG_NVM_PAR_T1, BMP388_NVM_PAR_T1);
    mclw_regmap_set_le16(map, 0, BMP388_REG_NVM_PAR_T2, BMP388_NVM_PAR_T2);
    mclw_regmap_set(map, 0, BMP388_REG_NVM_PAR_T3, 0, 0);
    mclw_regmap_set_le16(map, 0, BMP388_REG_NVM_PAR_P1, (uint16_t)BMP388_NVM_PAR_P1);
    mclw_regmap_set_le16(map, 0, BMP388_REG_NVM_PAR_P2, (uint16_t)BMP388_NVM_PAR_P2);
    for (uint8_t reg = BMP388_REG_NVM_PAR_P3; reg <= BMP388_REG_NVM_PAR_P11; reg++) {
        mclw_regmap_set(map, 0, reg, 0, 0);
    }
    // The part keeps a CRC-8 (polynomial 0x1D, seed 0xFF, final XOR 0xFF) of the
    // trimming block in NVM, readable at 0x30. Adafruit's BMP3XX driver (the
    // one Meshtastic uses) checks it in begin() and refuses a part whose CRC
    // does not match (validate_trimming_param in Adafruit_BMP3XX.cpp).
    uint8_t crc = 0xff;
    for (uint8_t reg = BMP388_REG_NVM_PAR_T1; reg <= BMP388_REG_NVM_PAR_P11; reg++) {
        uint8_t data = mclw_regmap_get(map, 0, reg);
        for (int bit = 0; bit < 8; bit++) {
            bool flip = ((crc ^ data) & 0x80u) != 0;
            crc = (uint8_t)(crc << 1);
            data = (uint8_t)(data << 1);
            if (flip) {
                crc ^= 0x1du;
            }
        }
    }
    mclw_regmap_set(map, 0, BMP388_REG_NVM_CRC, (uint8_t)(crc ^ 0xffu), 0);
}

static void build_map(struct bmp388_state *state) {
    struct mclw_regmap *map = &state->map;

    mclw_regmap_init(map, 1, 0, 0x00);
    mclw_regmap_set(map, 0, BMP388_REG_CHIP_ID, BMP388_CHIP_ID, 0);
    mclw_regmap_set(map, 0, BMP388_REG_ERR, BMP388_ERR_RESET, 0);
    mclw_regmap_set(map, 0, BMP388_REG_STATUS,
        BMP388_STATUS_CMD_RDY | BMP388_STATUS_DRDY_PRESS | BMP388_STATUS_DRDY_TEMP, 0);
    mclw_regmap_set(map, 0, BMP388_REG_EVENT, BMP388_EVENT_POR_DETECTED, 0);
    mclw_regmap_set(map, 0, BMP388_REG_INT_STATUS, BMP388_INT_STATUS_DRDY,
        MCLW_REG_CLEAR_ON_READ);
    mclw_regmap_set(map, 0, BMP388_REG_FIFO_WTM_0, BMP388_FIFO_WTM_0_RESET,
        MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, BMP388_REG_FIFO_WTM_1, 0, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, BMP388_REG_FIFO_CONFIG_1, BMP388_FIFO_CONFIG_RESET,
        MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, BMP388_REG_FIFO_CONFIG_2, BMP388_FIFO_CONFIG_RESET,
        MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, BMP388_REG_INT_CTRL, BMP388_INT_CTRL_RESET,
        MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, BMP388_REG_IF_CONF, 0, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, BMP388_REG_PWR_CTRL, 0, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, BMP388_REG_OSR, BMP388_OSR_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, BMP388_REG_ODR, 0, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, BMP388_REG_CONFIG, 0, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, BMP388_REG_CMD, 0, MCLW_REG_WRITABLE);
    build_calibration(map);
}

static void publish_sample(struct mclw_instance *inst) {
    struct bmp388_state *state = inst->state;
    uint32_t pressure = mclw_bmp388_raw_pressure(state->pressure_pa);
    uint32_t temperature = mclw_bmp388_raw_temperature(state->temperature_mc);
    uint32_t critical = mclw_hal_critical_enter();

    mclw_regmap_set_le24(&state->map, 0, BMP388_REG_DATA_0, pressure);
    mclw_regmap_set_le24(&state->map, 0, BMP388_REG_DATA_0 + 3, temperature);
    mclw_regmap_set(&state->map, 0, BMP388_REG_STATUS,
        BMP388_STATUS_CMD_RDY | BMP388_STATUS_DRDY_PRESS | BMP388_STATUS_DRDY_TEMP, 0);
    mclw_regmap_set(&state->map, 0, BMP388_REG_INT_STATUS, BMP388_INT_STATUS_DRDY,
        MCLW_REG_CLEAR_ON_READ);
    mclw_hal_critical_exit(critical);

    mclw_trace(inst, MCLW_TRACE_STATE, BMP388_REG_DATA_0, BMP388_SAMPLE_BYTES, 0);
}

static void disarm(struct bmp388_state *state) {
    if (state->i2c != NULL) {
        mclw_hal_i2c_target_disarm(state->i2c);
    }
    if (state->spi != NULL) {
        mclw_hal_spi_target_disarm(state->spi);
    }
}

static int arm(struct bmp388_state *state) {
    if (state->i2c != NULL) {
        return mclw_hal_i2c_target_arm(state->i2c, &state->map);
    }
    return mclw_hal_spi_target_arm_regmap(state->spi, &state->map, &bmp388_framing);
}

// The INT pad (3.7): data ready contributes while INT_STATUS holds it and
// drdy_en is set; unlatched, only for 2.5 ms after it asserted. The pin is
// driven push-pull at either polarity; int_od's open drain is not modelled.
static void drive_int(struct bmp388_state *state, uint64_t now_us) {
    uint8_t ctrl = mclw_regmap_get(&state->map, 0, BMP388_REG_INT_CTRL);
    bool status = (mclw_regmap_get(&state->map, 0, BMP388_REG_INT_STATUS) & BMP388_INT_STATUS_DRDY) != 0;
    bool active = (ctrl & BMP388_INT_CTRL_DRDY_EN) != 0 && status &&
        ((ctrl & BMP388_INT_CTRL_LATCH) != 0 || now_us < state->drdy_at_us + BMP388_DRDY_UNLATCHED_US);

    if (state->int_pin != NULL) {
        mclw_hal_gpio_write(state->int_pin, active == ((ctrl & BMP388_INT_CTRL_LEVEL) != 0));
    }
}

// Conversions (3.3): normal mode converts every 5 ms x 2^odr_sel while a
// sensor is enabled; forced mode converts once and returns to sleep. Each
// conversion publishes the sample and its data ready.
static void bmp388_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct bmp388_state *state = inst->state;
    uint8_t pwr = mclw_regmap_get(&state->map, 0, BMP388_REG_PWR_CTRL);
    uint8_t mode = (pwr >> BMP388_PWR_CTRL_MODE_POS) & BMP388_PWR_CTRL_MODE_MASK;
    bool enabled = (pwr & (BMP388_PWR_CTRL_PRESS_EN | BMP388_PWR_CTRL_TEMP_EN)) != 0;

    if (mode == BMP388_PWR_CTRL_MODE_NORMAL && enabled) {
        uint32_t odr = mclw_regmap_get(&state->map, 0, BMP388_REG_ODR) & BMP388_ODR_MASK;
        uint32_t period_us = BMP388_ODR_BASE_US << (odr > BMP388_ODR_SEL_MAX ? BMP388_ODR_SEL_MAX : odr);

        if (period_us != state->period_us) {
            state->period_us = period_us;
            state->next_us = now_us + period_us;
        } else if (now_us >= state->next_us) {
            publish_sample(inst);
            state->drdy_at_us = now_us;
            while (state->next_us <= now_us) {
                state->next_us += period_us;
            }
        }
    } else {
        state->period_us = 0;
        if (mode != BMP388_PWR_CTRL_MODE_SLEEP && mode != BMP388_PWR_CTRL_MODE_NORMAL && enabled) {
            publish_sample(inst);
            state->drdy_at_us = now_us;
            mclw_regmap_set(&state->map, 0, BMP388_REG_PWR_CTRL,
                (uint8_t)(pwr & ~(BMP388_PWR_CTRL_MODE_MASK << BMP388_PWR_CTRL_MODE_POS)), MCLW_REG_WRITABLE);
        }
    }
    drive_int(state, now_us);
}

static int publish_part(struct mclw_instance *inst) {
    struct bmp388_state *state = inst->state;

    disarm(state);
    build_map(state);
    publish_sample(inst);
    return arm(state);
}

static int bmp388_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct bmp388_state *state = inst->state;

    if (bindings[BINDING_I2C].present == bindings[BINDING_SPI].present) {
        return -EINVAL;
    }
    state->i2c = bindings[BINDING_I2C].present ? bindings[BINDING_I2C].i2c : NULL;
    state->spi = bindings[BINDING_SPI].present ? bindings[BINDING_SPI].spi : NULL;
    state->int_pin = bindings[BINDING_INT].present ? bindings[BINDING_INT].gpio : NULL;
    state->period_us = 0;
    state->drdy_at_us = 0;
    state->pressure_pa = BMP388_PRESSURE_DEFAULT_PA;
    state->temperature_mc = BMP388_TEMPERATURE_DEFAULT_MC;
    return publish_part(inst);
}

static void bmp388_deinit(struct mclw_instance *inst) {
    struct bmp388_state *state = inst->state;

    disarm(state);
    state->i2c = NULL;
    state->spi = NULL;
    state->int_pin = NULL;
}

static int bmp388_reset(struct mclw_instance *inst, bool factory) {
    struct bmp388_state *state = inst->state;

    (void)factory;
    state->pressure_pa = BMP388_PRESSURE_DEFAULT_PA;
    state->temperature_mc = BMP388_TEMPERATURE_DEFAULT_MC;
    return publish_part(inst);
}

static int bmp388_get(struct mclw_instance *inst, size_t property,
    struct mclw_value *out) {
    struct bmp388_state *state = inst->state;

    switch (property) {
        case PROP_PRESSURE:
            out->v[0] = state->pressure_pa;
            return 0;
        case PROP_TEMPERATURE:
            out->v[0] = state->temperature_mc;
            return 0;
        case PROP_POWER_MODE:
            out->v[0] = (mclw_regmap_get(&state->map, 0, BMP388_REG_PWR_CTRL)
                >> BMP388_PWR_CTRL_MODE_POS) & BMP388_PWR_CTRL_MODE_MASK;
            return 0;
        case PROP_OSR:
            out->v[0] = mclw_regmap_get(&state->map, 0, BMP388_REG_OSR) & BMP388_OSR_MASK;
            return 0;
        case PROP_ODR:
            out->v[0] = mclw_regmap_get(&state->map, 0, BMP388_REG_ODR) & BMP388_ODR_MASK;
            return 0;
        case PROP_SPI_MODE:
            return mclw_spi_mode_get(inst, BINDING_SPI, out);
        default:
            return -EINVAL;
    }
}

static int bmp388_set(struct mclw_instance *inst, size_t property,
    const struct mclw_value *in) {
    struct bmp388_state *state = inst->state;

    switch (property) {
        case PROP_PRESSURE:
            state->pressure_pa = (int32_t)in->v[0];
            break;
        case PROP_TEMPERATURE:
            state->temperature_mc = (int32_t)in->v[0];
            break;
        case PROP_SPI_MODE:
            return mclw_spi_mode_set(inst, BINDING_SPI, in);
        default:
            return -EINVAL;
    }
    publish_sample(inst);
    return 0;
}

static const struct mclw_model_ops bmp388_ops = {
    .init = bmp388_init,
    .deinit = bmp388_deinit,
    .reset = bmp388_reset,
    .get = bmp388_get,
    .set = bmp388_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = bmp388_time_advanced,
    .usb_rx = NULL,
};

const struct mclw_model mclw_model_bmp388 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "bmp388",
    .family = "bmp388",
    .config = NULL,
    .properties = bmp388_properties,
    .property_count = sizeof(bmp388_properties) / sizeof(bmp388_properties[0]),
    .regions = NULL,
    .region_count = 0,
    .faults = NULL,
    .fault_count = 0,
    .bindings = bmp388_bindings,
    .binding_count = sizeof(bmp388_bindings) / sizeof(bmp388_bindings[0]),
    .state_size = sizeof(struct bmp388_state),
    .ops = &bmp388_ops,
};
