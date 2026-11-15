// SPDX-License-Identifier: Apache-2.0 OR MIT
// TI INA219, INA228, INA237 and INA260 current/power monitors on I2C. One
// engine: the register layout behind each pointer value, the conversion
// schedule and the status flags. Each part's register table, ADC codes and
// arithmetic come from its datasheet, cited where they are used:
//   INA219  SBOS448G (Dec 2015)   INA228  SLYS021A (May 2022)
//   INA237  SBOSA20A (May 2022)   INA260  SBOS656C (Dec 2016)

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

#include "ina2xx.h"

enum ina2xx_kind {
    KIND_INA219,
    KIND_INA228,
    KIND_INA237,
    KIND_INA260,
};

// A model's properties are a prefix of this list: the INA260 has its shunt
// inside and no die temperature; the INA219 has no die temperature.
enum {
    PROP_BUS_VOLTAGE,
    PROP_CURRENT,
    PROP_SHUNT_RESISTANCE,
    PROP_TEMPERATURE,
};

enum {
    BINDING_I2C,
};

#define INA2XX_REGISTERS_MAX 20

// Every part: CONFIG at pointer 0, RST in bit 15, self-clearing.
#define PTR_CONFIG 0x00
#define CONFIG_RST 0x8000u

#define INA219_SHUNT 0x01
#define INA219_BUS 0x02
#define INA219_POWER 0x03
#define INA219_CURRENT 0x04
#define INA219_CALIBRATION 0x05

#define INA22X_ADC_CONFIG 0x01
#define INA22X_SHUNT_CAL 0x02
#define INA22X_VSHUNT 0x04
#define INA22X_VBUS 0x05
#define INA22X_DIETEMP 0x06
#define INA22X_CURRENT 0x07
#define INA22X_POWER 0x08
#define INA228_ENERGY 0x09
#define INA228_CHARGE 0x0a
#define INA22X_DIAG_ALRT 0x0b

#define INA260_CURRENT 0x01
#define INA260_BUS 0x02
#define INA260_POWER 0x03
#define INA260_MASK_ENABLE 0x06

// SBOS448G Figure 24: CNVR and OVF are the bus voltage register's bits 1, 0.
#define INA219_CNVR 0x0002u
#define INA219_OVF 0x0001u
// SLYS021A Table 7-16, SBOSA20A Table 7-13.
#define INA22X_ENERGYOF 0x0800u
#define INA22X_CHARGEOF 0x0400u
#define INA22X_MATHOF 0x0200u
#define INA22X_CNVRF 0x0002u
#define INA22X_MEMSTAT 0x0001u
// SLYS021A Table 7-5: RSTACC; ADCRANGE is bit 4 on both 22x parts.
#define INA228_RSTACC 0x4000u
#define INA22X_ADCRANGE 0x0010u
// SBOS656C Figure 34: CVRF bit 3, OVF bit 2.
#define INA260_CVRF 0x0008u
#define INA260_OVF 0x0004u

// The channels a conversion measures.
#define CH_SHUNT 0x1u
#define CH_BUS 0x2u
#define CH_TEMP 0x4u

enum ina2xx_mode {
    MODE_OFF,
    MODE_TRIGGERED,
    MODE_CONTINUOUS,
};

struct ina2xx_register {
    uint8_t pointer;
    // Bytes, most significant first over the bus.
    uint8_t width;
    uint16_t reset;
    // Bits the bus may write; the rest keep their reset value.
    uint16_t writable;
};

struct ina2xx_part {
    enum ina2xx_kind kind;
    const struct ina2xx_register *registers;
    uint8_t register_count;
    // The register whose write starts a conversion (CONFIG or ADC_CONFIG).
    uint8_t mode_pointer;
    // The register that holds the status flags, and which bits they are.
    uint8_t status_pointer;
    uint16_t status_mask;
    uint16_t status_reset;
    uint16_t cnvr;
    uint16_t ovf;
    // The default external shunt (the breakout the pico-examples program
    // assumes); 0 for a part with its shunt inside.
    int32_t shunt_uohm;
};

struct ina2xx_state {
    struct mclw_regmap map;
    uint8_t pointers[MCLW_REGMAP_BANK_SIZE];
    uint8_t address[INA2XX_REGISTERS_MAX];
    struct mclw_i2c_target *i2c;
    const struct ina2xx_part *part;
    // The world on the pins.
    int32_t bus_mv;
    int32_t current_ua;
    int32_t shunt_uohm;
    int32_t temperature_mc;
    // The last conversion's ADC codes (the INA260's shunt code is its
    // current code) and what the part calculated from them.
    int32_t vshunt;
    int32_t vbus;
    int32_t dietemp;
    int32_t current;
    uint32_t power;
    uint16_t status;
    // Set from on_write: the worker (re)starts the conversion schedule.
    bool trigger;
    bool converting;
    uint64_t due_us;
    // INA228 accumulators and their remainders in register-LSB microseconds.
    uint64_t energy;
    int64_t charge;
    uint64_t energy_rem;
    int64_t charge_rem;
};

// SBOS448G Table 2 and Figures 19-27.
static const struct ina2xx_register ina219_registers[] = {
    { PTR_CONFIG, 2, 0x399f, 0xffff },
    { INA219_SHUNT, 2, 0x0000, 0 },
    { INA219_BUS, 2, 0x0000, 0 },
    { INA219_POWER, 2, 0x0000, 0 },
    { INA219_CURRENT, 2, 0x0000, 0 },
    // FS0 is void and always reads 0 (Figure 27 note 1).
    { INA219_CALIBRATION, 2, 0x0000, 0xfffe },
};

// SLYS021A Table 7-3 and Tables 7-5..7-24.
static const struct ina2xx_register ina228_registers[] = {
    { PTR_CONFIG, 2, 0x0000, 0xfff0 },
    { INA22X_ADC_CONFIG, 2, 0xfb68, 0xffff },
    { INA22X_SHUNT_CAL, 2, 0x1000, 0x7fff },
    { 0x03, 2, 0x0000, 0x3fff },
    { INA22X_VSHUNT, 3, 0, 0 },
    { INA22X_VBUS, 3, 0, 0 },
    { INA22X_DIETEMP, 2, 0, 0 },
    { INA22X_CURRENT, 3, 0, 0 },
    { INA22X_POWER, 3, 0, 0 },
    { INA228_ENERGY, 5, 0, 0 },
    { INA228_CHARGE, 5, 0, 0 },
    { INA22X_DIAG_ALRT, 2, 0x0001, 0xf000 },
    { 0x0c, 2, 0x7fff, 0xffff },
    { 0x0d, 2, 0x8000, 0xffff },
    { 0x0e, 2, 0x7fff, 0x7fff },
    { 0x0f, 2, 0x0000, 0x7fff },
    { 0x10, 2, 0x7fff, 0xffff },
    { 0x11, 2, 0xffff, 0xffff },
    { 0x3e, 2, 0x5449, 0 },
    { 0x3f, 2, 0x2281, 0 },
};

// SBOSA20A Table 7-3 and Tables 7-5..7-20. CONFIG bits 14, 5 and 3-0 are
// reserved and read 0.
static const struct ina2xx_register ina237_registers[] = {
    { PTR_CONFIG, 2, 0x0000, 0xbfd0 },
    { INA22X_ADC_CONFIG, 2, 0xfb68, 0xffff },
    { INA22X_SHUNT_CAL, 2, 0x1000, 0x7fff },
    { INA22X_VSHUNT, 2, 0, 0 },
    { INA22X_VBUS, 2, 0, 0 },
    { INA22X_DIETEMP, 2, 0, 0 },
    { INA22X_CURRENT, 2, 0, 0 },
    { INA22X_POWER, 3, 0, 0 },
    { INA22X_DIAG_ALRT, 2, 0x0001, 0xf000 },
    { 0x0c, 2, 0x7fff, 0xffff },
    { 0x0d, 2, 0x8000, 0xffff },
    { 0x0e, 2, 0x7fff, 0x7fff },
    { 0x0f, 2, 0x0000, 0x7fff },
    { 0x10, 2, 0x7fff, 0xffff },
    { 0x11, 2, 0xffff, 0xffff },
    { 0x3e, 2, 0x5449, 0 },
};

// SBOS656C Table 4, Figures 30-37. CONFIG bits 14-12 read 110; Mask/Enable
// bits 9-5 are unused and 4-2 are flags.
static const struct ina2xx_register ina260_registers[] = {
    { PTR_CONFIG, 2, 0x6127, 0x8fff },
    { INA260_CURRENT, 2, 0, 0 },
    { INA260_BUS, 2, 0, 0 },
    { INA260_POWER, 2, 0, 0 },
    { INA260_MASK_ENABLE, 2, 0x0000, 0xfc03 },
    { 0x07, 2, 0x0000, 0xffff },
    { 0xfe, 2, 0x5449, 0 },
    { 0xff, 2, 0x2270, 0 },
};

#define COUNT(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

static const struct ina2xx_part ina219_part = {
    .kind = KIND_INA219,
    .registers = ina219_registers,
    .register_count = COUNT(ina219_registers),
    .mode_pointer = PTR_CONFIG,
    .status_pointer = INA219_BUS,
    .status_mask = INA219_CNVR | INA219_OVF,
    .status_reset = 0,
    .cnvr = INA219_CNVR,
    .ovf = INA219_OVF,
    // Adafruit's INA219 breakout, which ina219_i2c.c calibrates for.
    .shunt_uohm = 100000,
};

static const struct ina2xx_part ina228_part = {
    .kind = KIND_INA228,
    .registers = ina228_registers,
    .register_count = COUNT(ina228_registers),
    .mode_pointer = INA22X_ADC_CONFIG,
    .status_pointer = INA22X_DIAG_ALRT,
    .status_mask = INA22X_ENERGYOF | INA22X_CHARGEOF | INA22X_MATHOF | INA22X_CNVRF | INA22X_MEMSTAT,
    .status_reset = INA22X_MEMSTAT,
    .cnvr = INA22X_CNVRF,
    .ovf = INA22X_MATHOF,
    // The breakout's stock 15 mOhm (ina228_i2c.c SHUNT_BOARD_15M).
    .shunt_uohm = 15000,
};

static const struct ina2xx_part ina237_part = {
    .kind = KIND_INA237,
    .registers = ina237_registers,
    .register_count = COUNT(ina237_registers),
    .mode_pointer = INA22X_ADC_CONFIG,
    .status_pointer = INA22X_DIAG_ALRT,
    .status_mask = INA22X_MATHOF | INA22X_CNVRF | INA22X_MEMSTAT,
    .status_reset = INA22X_MEMSTAT,
    .cnvr = INA22X_CNVRF,
    .ovf = INA22X_MATHOF,
    // Adafruit's INA237 breakout (ina237_i2c.c SHUNT_RES_OHMS).
    .shunt_uohm = 15000,
};

static const struct ina2xx_part ina260_part = {
    .kind = KIND_INA260,
    .registers = ina260_registers,
    .register_count = COUNT(ina260_registers),
    .mode_pointer = PTR_CONFIG,
    .status_pointer = INA260_MASK_ENABLE,
    .status_mask = INA260_CVRF | INA260_OVF,
    .status_reset = 0,
    .cnvr = INA260_CVRF,
    .ovf = INA260_OVF,
    .shunt_uohm = 0,
};

static const struct mclw_property_desc ina2xx_properties[] = {
    {
        .name = "bus_voltage",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mV",
        .min = 0,
        .max = 85000,
    },
    {
        .name = "current",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "uA",
        .min = -100000000,
        .max = 100000000,
    },
    {
        .name = "shunt_resistance",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "uohm",
        .min = 1,
        .max = 10000000,
    },
    {
        .name = "temperature",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mC",
        .min = -40000,
        .max = 125000,
    },
};

static const struct mclw_binding_desc ina2xx_bindings[] = {
    {
        .name = "i2c",
        .kind = MCLW_BINDING_I2C_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
    },
};

static int64_t MCLW_RAM_FUNC(div_round)(int64_t n, int64_t d) {
    return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
}

static int64_t MCLW_RAM_FUNC(clamp)(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int MCLW_RAM_FUNC(find)(const struct ina2xx_part *part, uint8_t pointer) {
    for (uint8_t i = 0; i < part->register_count; i++) {
        if (part->registers[i].pointer == pointer) {
            return i;
        }
    }
    return -1;
}

static uint8_t MCLW_RAM_FUNC(byte_flags)(const struct ina2xx_part *part, uint8_t index, uint8_t byte) {
    const struct ina2xx_register *r = &part->registers[index];
    uint8_t flags = 0;

    if (r->width == 2 && (uint8_t)(r->writable >> (byte == 0 ? 8 : 0)) != 0) {
        flags |= MCLW_REG_WRITABLE;
    }
    // SBOSA20A 7.3.1: reading DIAG_ALRT clears CNVRF (bit 1, low byte).
    if (r->pointer == part->status_pointer && part->cnvr == INA22X_CNVRF && byte == 1) {
        flags |= MCLW_REG_CLEAR_ON_READ;
    }
    return flags;
}

static uint64_t MCLW_RAM_FUNC(get_reg)(const struct ina2xx_state *s, uint8_t pointer) {
    int index = find(s->part, pointer);
    uint64_t v = 0;

    if (index < 0) {
        return 0;
    }
    for (uint8_t b = 0; b < s->part->registers[index].width; b++) {
        v = (v << 8) | mclw_regmap_get(&s->map, 0, (uint8_t)(s->address[index] + b));
    }
    return v;
}

static void MCLW_RAM_FUNC(put_reg)(struct ina2xx_state *s, uint8_t pointer, uint64_t v) {
    int index = find(s->part, pointer);

    if (index < 0) {
        return;
    }
    uint8_t width = s->part->registers[index].width;
    for (uint8_t b = 0; b < width; b++) {
        uint8_t shift = (uint8_t)(8u * (width - 1u - b));
        mclw_regmap_set(&s->map, 0, (uint8_t)(s->address[index] + b), (uint8_t)(v >> shift),
            byte_flags(s->part, (uint8_t)index, b));
    }
}

// INA228/INA237: a read of DIAG_ALRT has cleared CNVRF in the image.
static void MCLW_RAM_FUNC(sync_status)(struct ina2xx_state *s) {
    if (s->part->cnvr == INA22X_CNVRF && (get_reg(s, s->part->status_pointer) & INA22X_CNVRF) == 0) {
        s->status &= (uint16_t)~INA22X_CNVRF;
    }
}

static void MCLW_RAM_FUNC(put_status)(struct ina2xx_state *s) {
    uint64_t v = get_reg(s, s->part->status_pointer);

    put_reg(s, s->part->status_pointer, (v & ~(uint64_t)s->part->status_mask) | s->status);
}

static uint8_t MCLW_RAM_FUNC(decode_mode)(const struct ina2xx_state *s, enum ina2xx_mode *mode) {
    uint8_t channels;
    unsigned m;

    if (s->part->mode_pointer == PTR_CONFIG) {
        // SBOS448G Table 6, SBOS656C Table 5: MODE[2:0]; bit 0 shunt, bit 1 bus.
        m = (unsigned)get_reg(s, PTR_CONFIG) & 0x7u;
        channels = (uint8_t)(m & 0x3u);
        *mode = channels == 0 ? MODE_OFF : (m < 4 ? MODE_TRIGGERED : MODE_CONTINUOUS);
        return channels;
    }
    // SLYS021A Table 7-6, SBOSA20A Table 7-6: MODE[15:12]; bit 0 bus,
    // bit 1 shunt, bit 2 temperature.
    m = (unsigned)(get_reg(s, INA22X_ADC_CONFIG) >> 12) & 0xfu;
    channels = (uint8_t)(((m & 1u) ? CH_BUS : 0u) | ((m & 2u) ? CH_SHUNT : 0u) | ((m & 4u) ? CH_TEMP : 0u));
    *mode = channels == 0 ? MODE_OFF : (m < 8 ? MODE_TRIGGERED : MODE_CONTINUOUS);
    return channels;
}

// One conversion cycle, averaging included.
static uint32_t MCLW_RAM_FUNC(conversion_us)(const struct ina2xx_state *s, uint8_t channels) {
    // SBOS448G Table 5: 9..12-bit single conversions, then 2..128 samples.
    static const uint32_t ina219_bits_us[4] = { 84, 148, 276, 532 };
    static const uint32_t ina219_avg_us[8] = { 532, 1060, 2130, 4260, 8510, 17020, 34050, 68100 };
    // SLYS021A / SBOSA20A Table 7-6 and SBOS656C Table 5.
    static const uint32_t ina22x_ct_us[8] = { 50, 84, 150, 280, 540, 1052, 2074, 4120 };
    static const uint32_t ina260_ct_us[8] = { 140, 204, 332, 588, 1100, 2116, 4156, 8244 };
    static const uint32_t averages[8] = { 1, 4, 16, 64, 128, 256, 512, 1024 };
    uint32_t total = 0;

    if (s->part->kind == KIND_INA219) {
        unsigned config = (unsigned)get_reg(s, PTR_CONFIG);
        unsigned sadc = (config >> 3) & 0xfu;
        unsigned badc = (config >> 7) & 0xfu;
        if (channels & CH_SHUNT) {
            total += (sadc & 8u) ? ina219_avg_us[sadc & 7u] : ina219_bits_us[sadc & 3u];
        }
        if (channels & CH_BUS) {
            total += (badc & 8u) ? ina219_avg_us[badc & 7u] : ina219_bits_us[badc & 3u];
        }
    } else if (s->part->kind == KIND_INA260) {
        unsigned config = (unsigned)get_reg(s, PTR_CONFIG);
        if (channels & CH_BUS) {
            total += ina260_ct_us[(config >> 6) & 7u];
        }
        if (channels & CH_SHUNT) {
            total += ina260_ct_us[(config >> 3) & 7u];
        }
        total *= averages[(config >> 9) & 7u];
    } else {
        unsigned adc = (unsigned)get_reg(s, INA22X_ADC_CONFIG);
        if (channels & CH_BUS) {
            total += ina22x_ct_us[(adc >> 9) & 7u];
        }
        if (channels & CH_SHUNT) {
            total += ina22x_ct_us[(adc >> 6) & 7u];
        }
        if (channels & CH_TEMP) {
            total += ina22x_ct_us[(adc >> 3) & 7u];
        }
        total *= averages[adc & 7u];
    }
    return total == 0 ? 1 : total;
}

// The world into ADC codes, for the channels converted.
static void MCLW_RAM_FUNC(convert)(struct ina2xx_state *s, uint8_t channels) {
    // Microamps through micro-ohms: picovolts across the shunt.
    int64_t shunt_pv = (int64_t)s->current_ua * s->shunt_uohm;
    int64_t bus_mv = s->bus_mv;
    unsigned config = (unsigned)get_reg(s, PTR_CONFIG);
    bool narrow = (config & INA22X_ADCRANGE) != 0;

    switch (s->part->kind) {
        case KIND_INA219: {
            // 10 uV/LSB, saturating at the PGA's range (Table 4, Table 7);
            // 4 mV/LSB up to the BRNG range (Figure 24).
            int64_t full = 4000 << ((config >> 11) & 3u);
            if (channels & CH_SHUNT) {
                s->vshunt = (int32_t)clamp(div_round(shunt_pv, 10000000), -full, full);
            }
            if (channels & CH_BUS) {
                s->vbus = (int32_t)clamp(div_round(bus_mv, 4), 0, (config & 0x2000u) ? 8000 : 4000);
            }
            break;
        }
        case KIND_INA228:
            // Table 8-1: 312.5 nV or 78.125 nV, 195.3125 uV, 7.8125 m°C per LSB;
            // VSHUNT and VBUS are 20-bit.
            if (channels & CH_SHUNT) {
                s->vshunt = (int32_t)clamp(div_round(shunt_pv, narrow ? 78125 : 312500), -524288, 524287);
            }
            if (channels & CH_BUS) {
                s->vbus = (int32_t)clamp(div_round(bus_mv * 128, 25), 0, 524287);
            }
            if (channels & CH_TEMP) {
                s->dietemp = (int32_t)clamp(div_round((int64_t)s->temperature_mc * 16, 125), INT16_MIN, INT16_MAX);
            }
            break;
        case KIND_INA237:
            // Table 8-1: 5 uV or 1.25 uV, 3.125 mV, 125 m°C (12 bits) per LSB.
            if (channels & CH_SHUNT) {
                s->vshunt = (int32_t)clamp(div_round(shunt_pv, narrow ? 1250000 : 5000000), INT16_MIN, INT16_MAX);
            }
            if (channels & CH_BUS) {
                s->vbus = (int32_t)clamp(div_round(bus_mv * 8, 25), 0, INT16_MAX);
            }
            if (channels & CH_TEMP) {
                s->dietemp = (int32_t)clamp(div_round(s->temperature_mc, 125), -2048, 2047);
            }
            break;
        case KIND_INA260:
            // Table 1: 1.25 mA and 1.25 mV per LSB; bus full scale 32767 (Figure 32).
            if (channels & CH_SHUNT) {
                s->vshunt = (int32_t)clamp(div_round(s->current_ua, 1250), INT16_MIN, INT16_MAX);
            }
            if (channels & CH_BUS) {
                s->vbus = (int32_t)clamp(div_round(bus_mv * 4, 5), 0, INT16_MAX);
            }
            break;
    }
}

// What the part calculates from the codes, into the result registers.
static void MCLW_RAM_FUNC(publish)(struct ina2xx_state *s) {
    int64_t current = 0;
    int64_t power = 0;
    bool overflow = false;

    switch (s->part->kind) {
        case KIND_INA219: {
            // Equation 4: shunt x CAL / 4096; Equation 5: current x bus / 5000.
            // Both read zero until CALIBRATION is programmed (8.5).
            int64_t cal = (int64_t)(get_reg(s, INA219_CALIBRATION) & 0xfffeu);
            current = s->vshunt * cal / 4096;
            overflow = current < INT16_MIN || current > INT16_MAX;
            current = clamp(current, INT16_MIN, INT16_MAX);
            power = (current < 0 ? -current : current) * s->vbus / 5000;
            overflow = overflow || power > UINT16_MAX;
            power = clamp(power, 0, UINT16_MAX);
            put_reg(s, INA219_SHUNT, (uint16_t)(int16_t)s->vshunt);
            put_reg(s, INA219_BUS, (uint64_t)s->vbus << 3);
            put_reg(s, INA219_CURRENT, (uint16_t)(int16_t)current);
            put_reg(s, INA219_POWER, (uint64_t)power);
            break;
        }
        case KIND_INA228:
        case KIND_INA237: {
            // SHUNT_CAL = 13107.2e6 (INA228) or 819.2e6 (INA237) x CURRENT_LSB x
            // RSHUNT, x4 at ADCRANGE 1 (SLYS021A Eq. 2, SBOSA20A Eq. 1), so
            // CURRENT = VSHUNT x 4096 / SHUNT_CAL on both, in both ranges. POWER
            // is 3.2 or 0.2 x CURRENT_LSB per LSB (Eq. 5, Eq. 4), so
            // |CURRENT| x VBUS / 16384 or / 64. Zero SHUNT_CAL reads zero (7.3.2).
            bool wide = s->part->kind == KIND_INA228;
            int64_t cal = (int64_t)(get_reg(s, INA22X_SHUNT_CAL) & 0x7fffu);
            int64_t limit = wide ? 524287 : INT16_MAX;
            current = cal != 0 ? (int64_t)s->vshunt * 4096 / cal : 0;
            overflow = current < -limit - 1 || current > limit;
            current = clamp(current, -limit - 1, limit);
            power = (current < 0 ? -current : current) * s->vbus / (wide ? 16384 : 64);
            overflow = overflow || power > 0xffffff;
            power = clamp(power, 0, 0xffffff);
            if (wide) {
                put_reg(s, INA22X_VSHUNT, ((uint64_t)(uint32_t)s->vshunt & 0xfffffu) << 4);
                put_reg(s, INA22X_VBUS, (uint64_t)s->vbus << 4);
                put_reg(s, INA22X_DIETEMP, (uint16_t)(int16_t)s->dietemp);
                put_reg(s, INA22X_CURRENT, ((uint64_t)current & 0xfffffu) << 4);
                put_reg(s, INA228_ENERGY, cal != 0 ? s->energy : 0);
                put_reg(s, INA228_CHARGE, cal != 0 ? (uint64_t)s->charge & 0xffffffffffu : 0);
            } else {
                put_reg(s, INA22X_VSHUNT, (uint16_t)(int16_t)s->vshunt);
                put_reg(s, INA22X_VBUS, (uint64_t)s->vbus);
                put_reg(s, INA22X_DIETEMP, (uint16_t)(s->dietemp * 16));
                put_reg(s, INA22X_CURRENT, (uint16_t)(int16_t)current);
            }
            put_reg(s, INA22X_POWER, (uint64_t)power);
            break;
        }
        case KIND_INA260:
            // 8.6.4: current x bus voltage, at 10 mW/LSB from 1.25 mA x 1.25 mV;
            // positive whichever way the current flows (8.5.1).
            current = s->vshunt;
            power = (current < 0 ? -current : current) * s->vbus / 6400;
            overflow = power > UINT16_MAX;
            power = clamp(power, 0, UINT16_MAX);
            put_reg(s, INA260_CURRENT, (uint16_t)(int16_t)s->vshunt);
            put_reg(s, INA260_BUS, (uint64_t)s->vbus);
            put_reg(s, INA260_POWER, (uint64_t)power);
            break;
    }
    s->current = (int32_t)current;
    s->power = (uint32_t)power;
    if (overflow) {
        s->status |= s->part->ovf;
    } else {
        s->status &= (uint16_t)~s->part->ovf;
    }
    put_status(s);
}

// SLYS021A 7.3.2, Eq. 6 and 7: ENERGY gains POWER x t / 16 and CHARGE
// gains CURRENT x t per conversion, t in seconds; both are 40-bit and roll
// over (8.1.2).
static void MCLW_RAM_FUNC(accumulate)(struct ina2xx_state *s, uint64_t elapsed_us) {
    const uint64_t wrap = 1ull << 40;

    if (s->part->kind != KIND_INA228 || (get_reg(s, INA22X_SHUNT_CAL) & 0x7fffu) == 0) {
        return;
    }
    s->energy_rem += (uint64_t)s->power * elapsed_us;
    s->energy += s->energy_rem / 16000000u;
    s->energy_rem %= 16000000u;
    if (s->energy >= wrap) {
        s->energy %= wrap;
        s->status |= INA22X_ENERGYOF;
    }
    s->charge_rem += (int64_t)s->current * (int64_t)elapsed_us;
    s->charge += s->charge_rem / 1000000;
    s->charge_rem %= 1000000;
    if (s->charge >= (int64_t)(wrap / 2) || s->charge < -(int64_t)(wrap / 2)) {
        s->charge = s->charge >= 0 ? s->charge - (int64_t)wrap : s->charge + (int64_t)wrap;
        s->status |= INA22X_CHARGEOF;
    }
}

static void MCLW_RAM_FUNC(clear_accumulators)(struct ina2xx_state *s) {
    s->energy = 0;
    s->charge = 0;
    s->energy_rem = 0;
    s->charge_rem = 0;
    s->status &= (uint16_t)~(INA22X_ENERGYOF | INA22X_CHARGEOF);
}

// A write to the mode register restarts conversions (SBOSA20A 7.3.1,
// SBOS448G CNVR, SBOS656C 8.6.1) and clears the conversion-ready flag. A
// continuous mode publishes at once; a triggered one when the worker has
// let the conversion time pass.
static void MCLW_RAM_FUNC(mode_written)(struct ina2xx_state *s) {
    enum ina2xx_mode mode;
    uint8_t channels = decode_mode(s, &mode);

    s->status &= (uint16_t)~s->part->cnvr;
    s->converting = false;
    s->trigger = mode != MODE_OFF;
    if (mode == MODE_CONTINUOUS) {
        convert(s, channels);
        publish(s);
    } else {
        put_status(s);
    }
}

static void MCLW_RAM_FUNC(reset_registers)(struct ina2xx_state *s) {
    const struct ina2xx_part *part = s->part;

    for (uint8_t i = 0; i < part->register_count; i++) {
        put_reg(s, part->registers[i].pointer, part->registers[i].reset);
    }
    s->status = part->status_reset;
    s->vshunt = 0;
    s->vbus = 0;
    s->dietemp = 0;
    clear_accumulators(s);
    mode_written(s);
}

static void MCLW_RAM_FUNC(ina2xx_on_write)(struct mclw_regmap *map, uint8_t bank, uint8_t addr,
    uint8_t value, void *ctx) {
    struct ina2xx_state *s = ctx;
    const struct ina2xx_part *part = s->part;
    int index = -1;

    (void)map;
    (void)bank;
    (void)value;
    // A register is written when its last writable byte is.
    for (uint8_t i = 0; i < part->register_count; i++) {
        const struct ina2xx_register *r = &part->registers[i];
        if (r->writable != 0 && addr == s->address[i] + ((r->writable & 0xffu) != 0 ? 1 : 0)) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return;
    }
    const struct ina2xx_register *r = &part->registers[index];
    uint16_t v = (uint16_t)((get_reg(s, r->pointer) & r->writable) | (r->reset & ~r->writable));

    if (r->pointer == PTR_CONFIG && (v & CONFIG_RST) != 0) {
        reset_registers(s);
        return;
    }
    put_reg(s, r->pointer, v);
    if (part->kind == KIND_INA228 && r->pointer == PTR_CONFIG && (v & INA228_RSTACC) != 0) {
        clear_accumulators(s);
    }
    sync_status(s);
    if (r->pointer == part->mode_pointer) {
        mode_written(s);
        return;
    }
    // A range or calibration change shows at once in a continuous mode.
    bool shapes_results = r->pointer == PTR_CONFIG
        || (part->kind == KIND_INA219 && r->pointer == INA219_CALIBRATION)
        || ((part->kind == KIND_INA228 || part->kind == KIND_INA237) && r->pointer == INA22X_SHUNT_CAL);
    enum ina2xx_mode mode;
    uint8_t channels = decode_mode(s, &mode);
    if (shapes_results && mode == MODE_CONTINUOUS) {
        convert(s, channels);
        publish(s);
    } else {
        put_status(s);
    }
}

// Registers in table order from address 0; every other pointer value reads
// the zero bytes after them.
static void layout(struct ina2xx_state *s) {
    const struct ina2xx_part *part = s->part;
    uint8_t next = 0;

    mclw_regmap_init(&s->map, 1, 0, 0x00);
    for (uint8_t i = 0; i < part->register_count; i++) {
        s->address[i] = next;
        next = (uint8_t)(next + part->registers[i].width);
    }
    for (size_t p = 0; p < MCLW_REGMAP_BANK_SIZE; p++) {
        s->pointers[p] = next;
    }
    for (uint8_t i = 0; i < part->register_count; i++) {
        s->pointers[part->registers[i].pointer] = s->address[i];
    }
    mclw_regmap_set_pointer_map(&s->map, s->pointers);
    mclw_regmap_set_on_write(&s->map, ina2xx_on_write, s);
}

static void defaults(struct ina2xx_state *s) {
    s->bus_mv = 0;
    s->current_ua = 0;
    s->shunt_uohm = s->part->shunt_uohm;
    s->temperature_mc = 25000;
}

static int ina2xx_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct ina2xx_state *s = inst->state;

    s->part = inst->model->config;
    s->i2c = bindings[BINDING_I2C].i2c;
    defaults(s);
    layout(s);
    uint32_t critical = mclw_hal_critical_enter();
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return mclw_hal_i2c_target_arm(s->i2c, &s->map);
}

static void ina2xx_deinit(struct mclw_instance *inst) {
    struct ina2xx_state *s = inst->state;

    mclw_hal_i2c_target_disarm(s->i2c);
    s->i2c = NULL;
}

static int ina2xx_reset(struct mclw_instance *inst, bool factory) {
    struct ina2xx_state *s = inst->state;

    (void)factory;
    uint32_t critical = mclw_hal_critical_enter();
    defaults(s);
    reset_registers(s);
    mclw_hal_critical_exit(critical);
    return 0;
}

static int ina2xx_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct ina2xx_state *s = inst->state;

    switch (property) {
        case PROP_BUS_VOLTAGE:
            out->v[0] = s->bus_mv;
            return 0;
        case PROP_CURRENT:
            out->v[0] = s->current_ua;
            return 0;
        case PROP_SHUNT_RESISTANCE:
            out->v[0] = s->shunt_uohm;
            return 0;
        case PROP_TEMPERATURE:
            out->v[0] = s->temperature_mc;
            return 0;
        default:
            return -EINVAL;
    }
}

static int ina2xx_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct ina2xx_state *s = inst->state;
    int32_t v = (int32_t)in->v[0];

    switch (property) {
        case PROP_BUS_VOLTAGE:
            s->bus_mv = v;
            break;
        case PROP_CURRENT:
            s->current_ua = v;
            break;
        case PROP_SHUNT_RESISTANCE:
            s->shunt_uohm = v;
            break;
        case PROP_TEMPERATURE:
            s->temperature_mc = v;
            break;
        default:
            return -EINVAL;
    }
    // A continuous mode shows the new world at once; a triggered one at its
    // next conversion.
    uint32_t critical = mclw_hal_critical_enter();
    enum ina2xx_mode mode;
    uint8_t channels = decode_mode(s, &mode);
    if (mode == MODE_CONTINUOUS) {
        convert(s, channels);
        publish(s);
    }
    mclw_hal_critical_exit(critical);
    mclw_trace(inst, MCLW_TRACE_STATE, (uint32_t)property, 1, 0);
    return 0;
}

// Conversion timing: a triggered conversion completes, and a continuous one
// sets the conversion-ready flag (and accumulates, on the INA228), once a
// cycle of conversion time has passed.
static void ina2xx_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct ina2xx_state *s = inst->state;
    uint32_t critical = mclw_hal_critical_enter();
    enum ina2xx_mode mode;
    uint8_t channels = decode_mode(s, &mode);
    uint64_t cycle = conversion_us(s, channels);

    sync_status(s);
    if (s->trigger) {
        s->trigger = false;
        s->converting = true;
        s->due_us = now_us + cycle;
    }
    if (s->converting && now_us >= s->due_us) {
        uint64_t cycles = mode == MODE_CONTINUOUS ? (now_us - s->due_us) / cycle + 1 : 1;
        convert(s, channels);
        publish(s);
        accumulate(s, cycles * cycle);
        s->status |= s->part->cnvr;
        if (mode == MODE_CONTINUOUS) {
            s->due_us += cycles * cycle;
        } else {
            s->converting = false;
        }
        publish(s);
    } else {
        // MEMSTAT shares DIAG_ALRT's low byte with CNVRF, which a read clears.
        put_status(s);
    }
    mclw_hal_critical_exit(critical);
}

static const struct mclw_model_ops ina2xx_ops = {
    .init = ina2xx_init,
    .deinit = ina2xx_deinit,
    .reset = ina2xx_reset,
    .get = ina2xx_get,
    .set = ina2xx_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = NULL,
    .write_region = NULL,
    .time_advanced = ina2xx_time_advanced,
    .poll = NULL,
    .usb_rx = NULL,
    .usb_control = NULL,
};

#define INA2XX_MODEL(symbol, name, part_config, properties_used) \
    const struct mclw_model symbol = { \
        .abi_version = MCLW_ABI_VERSION, \
        .id = name, \
        .family = "ina2xx", \
        .config = &part_config, \
        .properties = ina2xx_properties, \
        .property_count = properties_used, \
        .regions = NULL, \
        .region_count = 0, \
        .faults = NULL, \
        .fault_count = 0, \
        .bindings = ina2xx_bindings, \
        .binding_count = sizeof(ina2xx_bindings) / sizeof(ina2xx_bindings[0]), \
        .state_size = sizeof(struct ina2xx_state), \
        .ops = &ina2xx_ops, \
    }

INA2XX_MODEL(mclw_model_ina219, "ina219", ina219_part, PROP_SHUNT_RESISTANCE + 1);
INA2XX_MODEL(mclw_model_ina228, "ina228", ina228_part, PROP_TEMPERATURE + 1);
INA2XX_MODEL(mclw_model_ina237, "ina237", ina237_part, PROP_TEMPERATURE + 1);
INA2XX_MODEL(mclw_model_ina260, "ina260", ina260_part, PROP_CURRENT + 1);
