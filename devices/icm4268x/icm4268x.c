// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/bus.h"
#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "icm4268x.h"

enum {
    PROP_ACCEL,
    PROP_GYRO,
    PROP_TEMPERATURE,
    PROP_SPI_MODE,
    PROP_POWER_MODE,
    PROP_ACCEL_CONFIG,
    PROP_GYRO_CONFIG,
    PROP_BANK,
    PROP_RESETS,
    PROP_FIFO_COUNT,
    PROP_FIFO_LOST,
};

enum {
    BINDING_SPI,
    BINDING_INT1,
};

struct icm4268x_config {
    uint8_t who_am_i;
};

// INT_STATUS, FIFO_COUNTH and FIFO_COUNTL: a burst from any of them runs
// on into FIFO_DATA, as icm426xx's read of status, count and packet in one
// does.
#define FIFO_LEAD (ICM4268X_REG_FIFO_DATA - ICM4268X_REG_INT_STATUS)

struct icm4268x_state {
    struct mclw_regmap map;
    struct mclw_spi_target *spi;
    uint8_t who_am_i;
    int16_t accel[3];
    int16_t gyro[3];
    int16_t temperature;
    uint32_t resets;
    // The INT1 pin, when bound, and when the next sample is due.
    struct mclw_gpio *int1;
    uint64_t next_sample_us;
    // The FIFO: its bytes from the oldest, 0xFF past `fifo_fill` (an empty
    // FIFO reads 0xFF, FIFO_DATA's reset value), the size of the packets
    // it holds, packets dropped for want of room, when the next packet is
    // due and when the last one was (for a delta timestamp), and the ODR
    // codes the last packet was taken at.
    uint8_t window[FIFO_LEAD + ICM4268X_FIFO_BYTES];
    uint8_t *fifo;
    uint16_t fifo_fill;
    uint16_t fifo_lost;
    uint8_t fifo_packet;
    uint8_t fifo_accel_odr;
    uint8_t fifo_gyro_odr;
    uint64_t fifo_next_us;
    uint64_t fifo_last_us;
};

static const struct mclw_property_desc icm4268x_properties[] = {
    {
        .name = "accel",
        .type = MCLW_TYPE_I32,
        .count = 3,
        .access = MCLW_ACCESS_RW,
        .unit = "LSB",
        .min = INT16_MIN,
        .max = INT16_MAX,
    },
    {
        .name = "gyro",
        .type = MCLW_TYPE_I32,
        .count = 3,
        .access = MCLW_ACCESS_RW,
        .unit = "LSB",
        .min = INT16_MIN,
        .max = INT16_MAX,
    },
    {
        .name = "temperature",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "LSB",
        .min = INT16_MIN,
        .max = INT16_MAX,
    },
    MCLW_PROPERTY_SPI_MODE,
    {
        .name = "power_mode",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 0xff,
    },
    {
        .name = "accel_config",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 0xff,
    },
    {
        .name = "gyro_config",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 0xff,
    },
    {
        .name = "bank",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = ICM4268X_BANKS - 1,
    },
    {
        .name = "resets",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "fifo_count",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = "bytes",
        .min = 0,
        .max = ICM4268X_FIFO_BYTES,
    },
    {
        .name = "fifo_lost",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = "packets",
        .min = 0,
        .max = UINT16_MAX,
    },
};

static const struct mclw_binding_desc icm4268x_bindings[] = {
    {
        .name = "spi",
        .kind = MCLW_BINDING_SPI_TARGET,
        .required = true,
        .usb = NULL,
        .spi = {
            .modes = MCLW_SPI_MODE_BIT(0) | MCLW_SPI_MODE_BIT(3),
            .default_mode = 3,
            .msb_first = true,
        },
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
};

static const struct mclw_regmap_framing icm4268x_framing = {
    .read_bit_mask = ICM4268X_SPI_READ_BIT,
    .address_mask = ICM4268X_SPI_ADDRESS_MASK,
    .read_dummy_bytes = 0,
    .auto_increment = true,
};

static void MCLW_RAM_FUNC(load_sample)(struct icm4268x_state *state) {
    mclw_regmap_set_be16(&state->map, 0, ICM4268X_REG_TEMP_DATA1, (uint16_t)state->temperature);
    for (uint8_t i = 0; i < 3; i++) {
        mclw_regmap_set_be16(&state->map, 0, (uint8_t)(ICM4268X_REG_ACCEL_DATA_X1 + 2 * i), (uint16_t)state->accel[i]);
        mclw_regmap_set_be16(&state->map, 0, (uint8_t)(ICM4268X_REG_GYRO_DATA_X1 + 2 * i), (uint16_t)state->gyro[i]);
    }
}

// FIFO_COUNTH/L in bytes or records (FIFO_COUNT_REC) and in the byte order
// FIFO_COUNT_ENDIAN gives, and FIFO_LOST_PKT0/1 (low byte first, as 14.55
// and 14.56 describe them).
static void MCLW_RAM_FUNC(fifo_publish)(struct icm4268x_state *state) {
    struct mclw_regmap *map = &state->map;
    uint8_t intf = mclw_regmap_get(map, 0, ICM4268X_REG_INTF_CONFIG0);
    uint16_t count = state->fifo_fill;

    if ((intf & ICM4268X_INTF_CONFIG0_FIFO_COUNT_REC) != 0u) {
        count = state->fifo_packet != 0u ? (uint16_t)(count / state->fifo_packet) : 0u;
    }
    if ((intf & ICM4268X_INTF_CONFIG0_FIFO_COUNT_ENDIAN) != 0u) {
        mclw_regmap_set_be16(map, 0, ICM4268X_REG_FIFO_COUNTH, count);
    } else {
        mclw_regmap_set_le16(map, 0, ICM4268X_REG_FIFO_COUNTH, count);
    }
    mclw_regmap_set_le16(map, 0, ICM4268X_REG_FIFO_LOST_PKT0, state->fifo_lost);
}

static void MCLW_RAM_FUNC(fifo_flush)(struct icm4268x_state *state) {
    memset(state->fifo, 0xff, ICM4268X_FIFO_BYTES);
    state->fifo_fill = 0;
    state->fifo_next_us = 0;
    fifo_publish(state);
}

// A burst read of FIFO_DATA took `taken` bytes from the FIFO's start
// (the engine's stream, in interrupt context): they leave it. Without
// FIFO_RESUME_PARTIAL_RD a packet read in part is dropped whole.
static void MCLW_RAM_FUNC(fifo_on_read)(struct mclw_regmap *map, size_t taken, void *ctx) {
    struct icm4268x_state *state = ctx;
    size_t n = taken;

    if ((mclw_regmap_get(map, 0, ICM4268X_REG_FIFO_CONFIG1) & ICM4268X_FIFO_CONFIG1_RESUME_PARTIAL_RD) == 0u &&
        state->fifo_packet != 0u) {
        n = (n + state->fifo_packet - 1u) / state->fifo_packet * state->fifo_packet;
    }
    if (n > state->fifo_fill) {
        n = state->fifo_fill;
    }
    memmove(state->fifo, state->fifo + n, state->fifo_fill - n);
    memset(state->fifo + state->fifo_fill - n, 0xff, n);
    state->fifo_fill = (uint16_t)(state->fifo_fill - n);
    fifo_publish(state);
}

static uint8_t *put_be16(uint8_t *at, int32_t value) {
    at[0] = (uint8_t)((uint32_t)value >> 8);
    at[1] = (uint8_t)value;
    return at + 2;
}

// A 20-bit field's top 16 bits (Packet 4); its low four go after the
// packet's timestamp.
static uint8_t *put_20(uint8_t *at, int32_t value) {
    at[0] = (uint8_t)((uint32_t)value >> 12);
    at[1] = (uint8_t)((uint32_t)value >> 4);
    return at + 2;
}

// One packet into the FIFO, of the layout FIFO_CONFIG1 selects (6.1),
// taken at `at_us`; returns the INT_STATUS events it raised (FIFO_THS_INT,
// FIFO_FULL_INT). A sensor that is off fills its fields with the invalid
// value (-32768, or -524288 in 20 bits; 14.34). Called with the critical
// section held.
static uint8_t fifo_push(struct icm4268x_state *state, uint64_t at_us) {
    struct mclw_regmap *map = &state->map;
    uint8_t config1 = mclw_regmap_get(map, 0, ICM4268X_REG_FIFO_CONFIG1);
    uint8_t power = mclw_regmap_get(map, 0, ICM4268X_REG_PWR_MGMT0);
    uint8_t tmst = mclw_regmap_get(map, 0, ICM4268X_REG_TMST_CONFIG);
    bool accel_in = (config1 & ICM4268X_FIFO_CONFIG1_ACCEL_EN) != 0u;
    bool gyro_in = (config1 & ICM4268X_FIFO_CONFIG1_GYRO_EN) != 0u;
    bool hires = (config1 & ICM4268X_FIFO_CONFIG1_HIRES_EN) != 0u;
    bool accel_on = (power & ICM4268X_PWR_MGMT0_ACCEL_MODE_MASK) != 0u;
    bool gyro_on = (power & ICM4268X_PWR_MGMT0_GYRO_MODE_MASK) != 0u;
    uint8_t accel_odr = mclw_regmap_get(map, 0, ICM4268X_REG_ACCEL_CONFIG0) & ICM4268X_CONFIG0_ODR_MASK;
    uint8_t gyro_odr = mclw_regmap_get(map, 0, ICM4268X_REG_GYRO_CONFIG0) & ICM4268X_CONFIG0_ODR_MASK;
    uint16_t wm = (uint16_t)(mclw_regmap_get(map, 0, ICM4268X_REG_FIFO_CONFIG2) |
        ((mclw_regmap_get(map, 0, ICM4268X_REG_FIFO_CONFIG3) & 0x0fu) << 8));
    bool records = (mclw_regmap_get(map, 0, ICM4268X_REG_INTF_CONFIG0) & ICM4268X_INTF_CONFIG0_FIFO_COUNT_REC) != 0u;
    uint8_t packet[20];
    uint8_t *at = packet + 1;
    uint8_t size;
    uint8_t events = 0;
    uint32_t level_before;
    uint32_t level;
    uint32_t stamp;
    int16_t temperature8 = (int16_t)(state->temperature / 64);

    if (!accel_in && !gyro_in && !hires) {
        return 0;
    }
    size = hires ? 20u : (accel_in && gyro_in) ? 16u : 8u;
    packet[0] = (uint8_t)((hires || accel_in ? ICM4268X_FIFO_HEADER_ACCEL : 0u) |
        (hires || gyro_in ? ICM4268X_FIFO_HEADER_GYRO : 0u) | (hires ? ICM4268X_FIFO_HEADER_20 : 0u) |
        (size >= 16u ? ICM4268X_FIFO_HEADER_TIMESTAMP_ODR : 0u) |
        (accel_odr != state->fifo_accel_odr ? ICM4268X_FIFO_HEADER_ODR_ACCEL : 0u) |
        (gyro_odr != state->fifo_gyro_odr ? ICM4268X_FIFO_HEADER_ODR_GYRO : 0u));
    if (hires) {
        int32_t a[3];
        int32_t g[3];

        for (uint8_t i = 0; i < 3; i++) {
            a[i] = accel_on ? (int32_t)state->accel[i] * 4 : -524288;
            g[i] = gyro_on ? (int32_t)state->gyro[i] * 8 : -524288;
            at = put_20(at, a[i]);
        }
        for (uint8_t i = 0; i < 3; i++) {
            at = put_20(at, g[i]);
        }
        at = put_be16(at, state->temperature);
        at += 2;
        for (uint8_t i = 0; i < 3; i++) {
            *at++ = (uint8_t)(((uint32_t)a[i] & 0x0fu) << 4 | ((uint32_t)g[i] & 0x0fu));
        }
    } else {
        if (accel_in) {
            for (uint8_t i = 0; i < 3; i++) {
                at = put_be16(at, accel_on ? state->accel[i] : INT16_MIN);
            }
        }
        if (gyro_in) {
            for (uint8_t i = 0; i < 3; i++) {
                at = put_be16(at, gyro_on ? state->gyro[i] : INT16_MIN);
            }
        }
        *at++ = (uint8_t)temperature8;
    }
    if (size >= 16u) {
        uint64_t ticks = (tmst & ICM4268X_TMST_CONFIG_DELTA_EN) != 0u ? at_us - state->fifo_last_us : at_us;

        stamp = (uint32_t)(ticks / ((tmst & ICM4268X_TMST_CONFIG_RES_16US) != 0u ? 16u : 1u));
        put_be16(hires ? packet + 15 : packet + 14, (int32_t)(stamp & 0xffffu));
    }
    state->fifo_last_us = at_us;
    state->fifo_accel_odr = accel_odr;
    state->fifo_gyro_odr = gyro_odr;
    if (state->fifo_packet != size) {
        // A new layout: the packets of the old one are dropped.
        state->fifo_packet = size;
        memset(state->fifo, 0xff, ICM4268X_FIFO_BYTES);
        state->fifo_fill = 0;
    }
    level_before = records ? state->fifo_fill / size : state->fifo_fill;
    if (state->fifo_fill + size > ICM4268X_FIFO_BYTES) {
        if (state->fifo_lost < UINT16_MAX) {
            state->fifo_lost++;
        }
        events |= ICM4268X_INT_STATUS_FIFO_FULL;
    } else {
        memcpy(state->fifo + state->fifo_fill, packet, size);
        state->fifo_fill = (uint16_t)(state->fifo_fill + size);
        if (state->fifo_fill + size > ICM4268X_FIFO_BYTES) {
            events |= ICM4268X_INT_STATUS_FIFO_FULL;
        }
    }
    level = records ? state->fifo_fill / size : state->fifo_fill;
    if (wm != 0u && level >= wm &&
        (level_before < wm || (config1 & ICM4268X_FIFO_CONFIG1_WM_GT_TH) != 0u)) {
        events |= ICM4268X_INT_STATUS_FIFO_THS;
    }
    if (events != 0u) {
        mclw_regmap_set(map, 0, ICM4268X_REG_INT_STATUS, mclw_regmap_get(map, 0, ICM4268X_REG_INT_STATUS) | events,
            MCLW_REG_CLEAR_ON_READ);
    }
    fifo_publish(state);
    return events;
}

// Power-on state, as the part leaves it after a soft reset: every register
// writable and zero except the few with a documented reset value.
static void MCLW_RAM_FUNC(build_map)(struct icm4268x_state *state) {
    struct mclw_regmap *map = &state->map;

    for (uint8_t bank = 0; bank < ICM4268X_BANKS; bank++) {
        memset(map->value[bank], 0, MCLW_REGMAP_BANK_SIZE);
        memset(map->flags[bank], MCLW_REG_WRITABLE, MCLW_REGMAP_BANK_SIZE);
    }
    map->active_bank = 0;
    mclw_regmap_set(map, 0, ICM4268X_REG_WHO_AM_I, state->who_am_i, 0);
    // INT_STATUS clears when read (14.21).
    mclw_regmap_set(map, 0, ICM4268X_REG_INT_STATUS, ICM4268X_INT_STATUS_RESET_DONE, MCLW_REG_CLEAR_ON_READ);
    mclw_regmap_set(map, 0, ICM4268X_REG_INT_SOURCE0, ICM4268X_INT_SOURCE0_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, ICM4268X_REG_TMST_CONFIG, ICM4268X_TMST_CONFIG_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, ICM4268X_REG_INTF_CONFIG1, ICM4268X_INTF_CONFIG1_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, ICM4268X_REG_INT_CONFIG1, ICM4268X_INT_CONFIG1_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, ICM4268X_REG_ACCEL_CONFIG0, ICM4268X_ACCEL_CONFIG0_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, ICM4268X_REG_GYRO_CONFIG0, ICM4268X_GYRO_CONFIG0_RESET, MCLW_REG_WRITABLE);
    for (uint8_t reg = ICM4268X_REG_TEMP_DATA1; reg < ICM4268X_REG_TEMP_DATA1 + ICM4268X_SAMPLE_BYTES; reg++) {
        mclw_regmap_set(map, 0, reg, 0, 0);
    }
    mclw_regmap_set(map, 0, ICM4268X_REG_INTF_CONFIG0, ICM4268X_INTF_CONFIG0_RESET, MCLW_REG_WRITABLE);
    mclw_regmap_set(map, 0, ICM4268X_REG_FIFO_DATA, 0xff, 0);
    for (uint8_t reg = ICM4268X_REG_FIFO_COUNTH; reg <= ICM4268X_REG_FIFO_COUNTL; reg++) {
        mclw_regmap_set(map, 0, reg, 0, 0);
    }
    for (uint8_t reg = ICM4268X_REG_FIFO_LOST_PKT0; reg <= ICM4268X_REG_FIFO_LOST_PKT1; reg++) {
        mclw_regmap_set(map, 0, reg, 0, 0);
    }
    load_sample(state);
    state->fifo_lost = 0;
    state->fifo_packet = 0;
    fifo_flush(state);
}

static void MCLW_RAM_FUNC(raise_data_ready)(struct icm4268x_state *state) {
    struct mclw_regmap *map = &state->map;
    if (mclw_regmap_get(map, 0, ICM4268X_REG_PWR_MGMT0) & ICM4268X_PWR_MGMT0_SENSOR_MASK) {
        mclw_regmap_set(map, 0, ICM4268X_REG_INT_STATUS,
            mclw_regmap_get(map, 0, ICM4268X_REG_INT_STATUS) | ICM4268X_INT_STATUS_DATA_RDY,
            MCLW_REG_CLEAR_ON_READ);
    }
}

// Interrupt context, after the engine committed a bus write and before it
// re-arms, so it and everything it calls are in RAM.
static void MCLW_RAM_FUNC(on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t reg, uint8_t value, void *ctx) {
    struct icm4268x_state *state = ctx;
    (void)map;

    if (bank != 0) {
        return;
    }
    if (reg == ICM4268X_REG_DEVICE_CONFIG && (value & ICM4268X_DEVICE_CONFIG_SOFT_RESET)) {
        build_map(state);
        state->resets++;
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_COMMAND, reg, 1, 0);
    } else if (reg == ICM4268X_REG_PWR_MGMT0) {
        raise_data_ready(state);
    } else if (reg == ICM4268X_REG_SIGNAL_PATH_RESET) {
        // Write-to-clear (14.33): the bits act and read back 0.
        mclw_regmap_set(&state->map, 0, reg, 0, MCLW_REG_WRITABLE);
        if ((value & ICM4268X_SIGNAL_PATH_RESET_FIFO_FLUSH) != 0u) {
            fifo_flush(state);
        }
    } else if (reg == ICM4268X_REG_FIFO_CONFIG && (value & ICM4268X_FIFO_MODE_MASK) == 0u) {
        fifo_flush(state);
    } else if (reg == ICM4268X_REG_INTF_CONFIG0) {
        fifo_publish(state);
    }
}

static int icm4268x_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct icm4268x_state *state = inst->state;

    state->spi = bindings[BINDING_SPI].spi;
    state->int1 = bindings[BINDING_INT1].present ? bindings[BINDING_INT1].gpio : NULL;
    state->next_sample_us = 0;
    state->who_am_i = ((const struct icm4268x_config *)inst->model->config)->who_am_i;
    mclw_regmap_init(&state->map, ICM4268X_BANKS, ICM4268X_REG_BANK_SEL, 0x00);
    mclw_regmap_set_on_write(&state->map, on_write, state);
    state->fifo = state->window + FIFO_LEAD;
    build_map(state);
    mclw_regmap_set_stream(&state->map, 0, ICM4268X_REG_FIFO_DATA, FIFO_LEAD, state->window, ICM4268X_FIFO_BYTES,
        fifo_on_read, state);
    return mclw_hal_spi_target_arm_regmap(state->spi, &state->map, &icm4268x_framing);
}

static void icm4268x_deinit(struct mclw_instance *inst) {
    struct icm4268x_state *state = inst->state;

    mclw_hal_spi_target_disarm(state->spi);
    state->spi = NULL;
    state->int1 = NULL;
}

static int icm4268x_reset(struct mclw_instance *inst, bool factory) {
    struct icm4268x_state *state = inst->state;
    uint32_t critical;

    (void)factory;
    memset(state->accel, 0, sizeof(state->accel));
    memset(state->gyro, 0, sizeof(state->gyro));
    state->temperature = 0;
    critical = mclw_hal_critical_enter();
    build_map(state);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int icm4268x_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct icm4268x_state *state = inst->state;

    switch (property) {
        case PROP_ACCEL:
        case PROP_GYRO: {
            const int16_t *axes = property == PROP_ACCEL ? state->accel : state->gyro;
            for (size_t i = 0; i < 3; i++) {
                out->v[i] = axes[i];
            }
            return 0;
        }
        case PROP_TEMPERATURE:
            out->v[0] = state->temperature;
            return 0;
        case PROP_SPI_MODE:
            return mclw_spi_mode_get(inst, BINDING_SPI, out);
        case PROP_POWER_MODE:
            out->v[0] = mclw_regmap_get(&state->map, 0, ICM4268X_REG_PWR_MGMT0);
            return 0;
        case PROP_ACCEL_CONFIG:
            out->v[0] = mclw_regmap_get(&state->map, 0, ICM4268X_REG_ACCEL_CONFIG0);
            return 0;
        case PROP_GYRO_CONFIG:
            out->v[0] = mclw_regmap_get(&state->map, 0, ICM4268X_REG_GYRO_CONFIG0);
            return 0;
        case PROP_BANK:
            out->v[0] = state->map.active_bank;
            return 0;
        case PROP_RESETS:
            out->v[0] = state->resets;
            return 0;
        case PROP_FIFO_COUNT:
            out->v[0] = state->fifo_fill;
            return 0;
        case PROP_FIFO_LOST:
            out->v[0] = state->fifo_lost;
            return 0;
        default:
            return -EINVAL;
    }
}

static int icm4268x_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct icm4268x_state *state = inst->state;
    uint32_t critical;

    switch (property) {
        case PROP_ACCEL:
        case PROP_GYRO:
        case PROP_TEMPERATURE:
            break;
        case PROP_SPI_MODE:
            return mclw_spi_mode_set(inst, BINDING_SPI, in);
        default:
            return -EINVAL;
    }
    // The state and the image change together: a soft reset in on_write
    // rebuilds the image from the state, so it must not run between them.
    critical = mclw_hal_critical_enter();
    if (property == PROP_TEMPERATURE) {
        state->temperature = (int16_t)in->v[0];
    } else {
        int16_t *axes = property == PROP_ACCEL ? state->accel : state->gyro;

        for (size_t i = 0; i < 3; i++) {
            axes[i] = (int16_t)in->v[i];
        }
    }
    load_sample(state);
    raise_data_ready(state);
    mclw_hal_critical_exit(critical);
    mclw_trace(inst, MCLW_TRACE_STATE, ICM4268X_REG_TEMP_DATA1, ICM4268X_SAMPLE_BYTES, 0);
    return 0;
}

// The sample period of an ACCEL_CONFIG0 or GYRO_CONFIG0 ODR code (14.37,
// 14.38), in microseconds; 0 for the reserved code.
static uint32_t odr_period_us(uint8_t code) {
    static const uint32_t period_us[16] = {
        0, 31, 63, 125, 250, 500, 1000, 5000, 10000, 20000, 40000, 80000, 160000, 320000, 640000, 2000,
    };

    return period_us[code & ICM4268X_CONFIG0_ODR_MASK];
}

// INT1 at `level`: push-pull drives it either way; open drain (14.3,
// INT1_DRIVE_CIRCUIT 0, the reset value) only pulls it low and otherwise
// lets go.
static void write_int1(struct icm4268x_state *state, bool level) {
    if (level && (mclw_regmap_get(&state->map, 0, ICM4268X_REG_INT_CONFIG) & ICM4268X_INT_CONFIG_INT1_PUSH_PULL) == 0) {
        mclw_hal_gpio_release(state->int1);
    } else {
        mclw_hal_gpio_write(state->int1, level);
    }
}

#define INT1_EVENTS (ICM4268X_INT_STATUS_DATA_RDY | ICM4268X_INT_STATUS_FIFO_THS | ICM4268X_INT_STATUS_FIFO_FULL)

// Drives INT1 (14.3, 14.50, 14.51): each of `events` (INT_STATUS bits)
// that INT_SOURCE0 routes to INT1 (UI_DRDY, FIFO_THS, FIFO_FULL) emits a
// 100 us pulse (8 us with INT_TPULSE_DURATION), or in latched mode holds
// INT1 until INT_STATUS is read; INT1_POLARITY sets the active level.
static void drive_int1(struct icm4268x_state *state, uint8_t events) {
    struct mclw_regmap *map = &state->map;
    uint8_t config = mclw_regmap_get(map, 0, ICM4268X_REG_INT_CONFIG);
    bool high = (config & ICM4268X_INT_CONFIG_INT1_POLARITY_HIGH) != 0;
    bool latched = (config & ICM4268X_INT_CONFIG_INT1_MODE_LATCHED) != 0;
    // INT_SOURCE0's routing bits sit where INT_STATUS has their events.
    uint8_t routed = mclw_regmap_get(map, 0, ICM4268X_REG_INT_SOURCE0) & INT1_EVENTS;
    bool pending = (mclw_regmap_get(map, 0, ICM4268X_REG_INT_STATUS) & routed) != 0;

    if (state->int1 == NULL) {
        return;
    }
    if ((events & routed) != 0u && !latched) {
        bool short_pulse = (mclw_regmap_get(map, 0, ICM4268X_REG_INT_CONFIG1) & ICM4268X_INT_CONFIG1_TPULSE_8US) != 0;

        write_int1(state, high);
        mclw_hal_delay_us(short_pulse ? 8u : 100u);
    }
    write_int1(state, (latched && pending) == high);
}

// The packets one tick adds at most, and the lag past which the FIFO stops
// catching up and starts again from now.
#define FIFO_PACKETS_PER_TICK 32u
#define FIFO_CATCH_UP_US 20000u

// A new sample at the ODR of the accelerometer, or the gyroscope when only
// it runs, once per tick at most: the simulation clock ticks every
// millisecond, so ODRs above 1 kHz deliver at 1 kHz.
static void icm4268x_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct icm4268x_state *state = inst->state;
    struct mclw_regmap *map = &state->map;
    uint8_t power = mclw_regmap_get(map, 0, ICM4268X_REG_PWR_MGMT0);
    uint8_t code = (power & ICM4268X_PWR_MGMT0_ACCEL_MODE_MASK) != 0
        ? mclw_regmap_get(map, 0, ICM4268X_REG_ACCEL_CONFIG0)
        : mclw_regmap_get(map, 0, ICM4268X_REG_GYRO_CONFIG0);
    uint32_t period_us = odr_period_us(code);
    bool running = (power & ICM4268X_PWR_MGMT0_SENSOR_MASK) != 0 && period_us != 0;
    uint8_t events = 0;

    if (running && now_us >= state->next_sample_us) {
        uint32_t critical = mclw_hal_critical_enter();

        load_sample(state);
        raise_data_ready(state);
        mclw_hal_critical_exit(critical);
        state->next_sample_us = now_us + period_us;
        events |= ICM4268X_INT_STATUS_DATA_RDY;
    }
    // The FIFO takes every ODR's packet, so above 1 kHz a tick adds
    // several, timestamped a period apart.
    if (running && (mclw_regmap_get(map, 0, ICM4268X_REG_FIFO_CONFIG) & ICM4268X_FIFO_MODE_MASK) != 0u) {
        if (state->fifo_next_us == 0u ||
            (state->fifo_next_us <= now_us && now_us - state->fifo_next_us > FIFO_CATCH_UP_US)) {
            state->fifo_next_us = now_us;
        }
        for (uint32_t n = 0; n < FIFO_PACKETS_PER_TICK && state->fifo_next_us <= now_us; n++) {
            uint32_t critical = mclw_hal_critical_enter();

            events |= fifo_push(state, state->fifo_next_us);
            mclw_hal_critical_exit(critical);
            state->fifo_next_us += period_us;
        }
    }
    drive_int1(state, events);
}

static const struct mclw_model_ops icm4268x_ops = {
    .init = icm4268x_init,
    .deinit = icm4268x_deinit,
    .reset = icm4268x_reset,
    .get = icm4268x_get,
    .set = icm4268x_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = icm4268x_time_advanced,
    .usb_rx = NULL,
};

#define ICM4268X_MODEL(symbol, name, who) \
    static const struct icm4268x_config symbol##_config = { .who_am_i = who }; \
    const struct mclw_model symbol = { \
        .abi_version = MCLW_ABI_VERSION, \
        .id = name, \
        .family = "icm4268x", \
        .config = &symbol##_config, \
        .properties = icm4268x_properties, \
        .property_count = sizeof(icm4268x_properties) / sizeof(icm4268x_properties[0]), \
        .regions = NULL, \
        .region_count = 0, \
        .faults = NULL, \
        .fault_count = 0, \
        .bindings = icm4268x_bindings, \
        .binding_count = sizeof(icm4268x_bindings) / sizeof(icm4268x_bindings[0]), \
        .state_size = sizeof(struct icm4268x_state), \
        .ops = &icm4268x_ops, \
    }

ICM4268X_MODEL(mclw_model_icm42688, "icm42688", ICM4268X_WHO_AM_I_ICM42688);
ICM4268X_MODEL(mclw_model_icm42686, "icm42686", ICM4268X_WHO_AM_I_ICM42686);
