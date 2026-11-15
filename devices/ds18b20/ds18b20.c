// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/wire.h"
#include "metalclaw/model.h"
#include "metalclaw/onewire.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "ds18b20.h"

enum {
    PROP_TEMPERATURE,
    PROP_SERIAL,
    PROP_ROM,
    PROP_RESOLUTION,
    PROP_TH,
    PROP_TL,
    PROP_ALARM,
    PROP_CONVERSIONS,
    PROP_SCRATCHPAD_READS,
    PROP_SEARCHES,
    PROP_MATCHES,
    PROP_RESETS,
};

enum {
    BINDING_DQ,
};

enum {
    REGION_SCRATCHPAD,
    REGION_ROM,
};

// Function commands (Table 3), in this order in the image.
enum {
    FN_CONVERT,
    FN_READ,
    FN_WRITE,
    FN_COPY,
    FN_RECALL,
    FN_POWER,
};

struct ds18b20_state {
    struct mclw_onewire bus;
    struct mclw_wire *dq;
    int32_t temperature_mc;
    uint32_t serial;
    // TH, TL and the configuration register (Figure 7, EEPROM column).
    uint8_t eeprom[3];
    bool converting;
    uint64_t converted_at_us;
    uint32_t conversions;
    uint32_t scratchpad_reads;
};

static const struct mclw_onewire_function ds18b20_functions[] = {
    [FN_CONVERT] = { .command = DS18B20_CONVERT_T, .flags = MCLW_ONEWIRE_STATUS | MCLW_ONEWIRE_BUSY },
    [FN_READ] = { .command = DS18B20_READ_SCRATCHPAD, .tx_offset = 0, .tx_length = DS18B20_SCRATCHPAD_BYTES },
    [FN_WRITE] = { .command = DS18B20_WRITE_SCRATCHPAD, .rx_length = 3 },
    [FN_COPY] = { .command = DS18B20_COPY_SCRATCHPAD },
    [FN_RECALL] = { .command = DS18B20_RECALL_E2, .flags = MCLW_ONEWIRE_STATUS | MCLW_ONEWIRE_BUSY },
    // An externally powered part leaves the bus high in the read slot.
    [FN_POWER] = { .command = DS18B20_READ_POWER_SUPPLY },
};

#define COUNTER(property_name) \
    { \
        .name = (property_name), \
        .type = MCLW_TYPE_U32, \
        .count = 1, \
        .access = MCLW_ACCESS_RO, \
        .unit = NULL, \
        .min = 0, \
        .max = UINT32_MAX, \
    }

static const struct mclw_property_desc ds18b20_properties[] = {
    {
        .name = "temperature",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "mC",
        .min = DS18B20_TEMPERATURE_MIN_MC,
        .max = DS18B20_TEMPERATURE_MAX_MC,
    },
    {
        .name = "serial",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "rom",
        .type = MCLW_TYPE_U32,
        .count = 2,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = UINT32_MAX,
    },
    {
        .name = "resolution",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = "bits",
        .min = 9,
        .max = 12,
    },
    {
        .name = "th",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = "C",
        .min = -128,
        .max = 127,
    },
    {
        .name = "tl",
        .type = MCLW_TYPE_I32,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = "C",
        .min = -128,
        .max = 127,
    },
    {
        .name = "alarm",
        .type = MCLW_TYPE_BOOL,
        .count = 1,
        .access = MCLW_ACCESS_RO,
        .unit = NULL,
        .min = 0,
        .max = 1,
    },
    COUNTER("conversions"),
    COUNTER("scratchpad_reads"),
    COUNTER("searches"),
    COUNTER("matches"),
    COUNTER("resets"),
};

static const struct mclw_region_desc ds18b20_regions[] = {
    { .name = "scratchpad", .size = DS18B20_SCRATCHPAD_BYTES, .access = MCLW_ACCESS_RO },
    { .name = "rom", .size = 8, .access = MCLW_ACCESS_RO },
};

// Figure 14: the part samples a write slot 15 to 60 us after its falling
// edge, typically 30 us, and holds a 0 for the master to sample within
// 15 us. Figure 13: it waits 15 to 60 us after a reset's rising edge, then
// pulls the bus low for 60 to 240 us.
static const struct mclw_binding_desc ds18b20_bindings[] = {
    {
        .name = "dq",
        .kind = MCLW_BINDING_WIRE,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
        .wire = {
            .engine = MCLW_WIRE_ONEWIRE,
            .pins = 1,
            .onewire = { .sample_ns = 30000, .presence_wait_ns = 30000, .presence_ns = 120000 },
        },
    },
};

static uint8_t config_of(const struct ds18b20_state *state) {
    return state->bus.memory[DS18B20_SP_CONFIG];
}

uint32_t ds18b20_resolution_bits(uint8_t config) {
    return 9u + ((config >> 5) & 3u);
}

// Table 2: 93.75 ms at 9 bits, doubling with each bit to 750 ms at 12.
uint32_t ds18b20_conversion_us(uint8_t config) {
    return 93750u << ((config >> 5) & 3u);
}

// Figure 2: 1/16 degC per count, two's complement; the bits a coarser
// resolution leaves undefined are 0 here. Rounded to the nearest count.
int16_t ds18b20_temperature_word(int32_t milli_c, uint8_t config) {
    int32_t sixteenths = milli_c >= 0 ? (milli_c * 16 + 500) / 1000 : -((-milli_c * 16 + 500) / 1000);
    uint32_t undefined = 3u - ((config >> 5) & 3u);

    return (int16_t)(sixteenths & ~(int32_t)((1u << undefined) - 1u));
}

static void refresh_crc(struct ds18b20_state *state) {
    state->bus.memory[DS18B20_SP_CRC] = mclw_onewire_crc8(state->bus.memory, DS18B20_SP_CRC);
}

// Figure 6: family code 28h, a 48-bit serial number (the property's 32
// bits, the upper 16 zero), and the CRC of those 56 bits.
static void build_rom(uint32_t serial, uint8_t rom[8]) {
    rom[0] = DS18B20_FAMILY;
    rom[1] = (uint8_t)serial;
    rom[2] = (uint8_t)(serial >> 8);
    rom[3] = (uint8_t)(serial >> 16);
    rom[4] = (uint8_t)(serial >> 24);
    rom[5] = 0;
    rom[6] = 0;
    rom[7] = mclw_onewire_crc8(rom, 7);
}

static void recall(struct ds18b20_state *state) {
    memcpy(&state->bus.memory[DS18B20_SP_TH], state->eeprom, sizeof(state->eeprom));
    refresh_crc(state);
}

// The alarm compares bits 11..4 of the temperature with TH and TL, and is
// set at or beyond either ("OPERATION—ALARM SIGNALING").
static void finish_conversion(struct ds18b20_state *state) {
    int16_t word = ds18b20_temperature_word(state->temperature_mc, config_of(state));
    int8_t whole = (int8_t)(word >> 4);
    int8_t th = (int8_t)state->bus.memory[DS18B20_SP_TH];
    int8_t tl = (int8_t)state->bus.memory[DS18B20_SP_TL];

    state->bus.memory[DS18B20_SP_TEMP_LSB] = (uint8_t)word;
    state->bus.memory[DS18B20_SP_TEMP_MSB] = (uint8_t)((uint16_t)word >> 8);
    refresh_crc(state);
    state->bus.alarm = whole >= th || whole <= tl;
    state->bus.status = true;
    state->converting = false;
    state->conversions++;
}

static void ds18b20_poll(struct mclw_instance *inst) {
    struct ds18b20_state *state = inst->state;
    uint8_t rx[MCLW_ONEWIRE_RX_MAX];
    uint8_t rx_count = 0;
    uint32_t critical = mclw_hal_critical_enter();
    uint32_t issued = mclw_onewire_take(&state->bus, rx, &rx_count);

    if ((issued & (1u << FN_WRITE)) != 0u && rx_count >= 3u) {
        state->bus.memory[DS18B20_SP_TH] = rx[0];
        state->bus.memory[DS18B20_SP_TL] = rx[1];
        state->bus.memory[DS18B20_SP_CONFIG] = (uint8_t)((rx[2] & DS18B20_CONFIG_WRITABLE) | DS18B20_CONFIG_FIXED);
        refresh_crc(state);
    }
    if ((issued & (1u << FN_COPY)) != 0u) {
        memcpy(state->eeprom, &state->bus.memory[DS18B20_SP_TH], sizeof(state->eeprom));
    }
    if ((issued & (1u << FN_RECALL)) != 0u) {
        recall(state);
        state->bus.status = true;
    }
    mclw_hal_critical_exit(critical);
    if ((issued & (1u << FN_CONVERT)) != 0u) {
        state->converting = true;
        state->converted_at_us = mclw_now_us() + ds18b20_conversion_us(config_of(state));
        mclw_trace(inst, MCLW_TRACE_COMMAND, DS18B20_CONVERT_T, 0, 0);
    }
    if ((issued & (1u << FN_READ)) != 0u) {
        state->scratchpad_reads++;
    }
}

// The conversion runs on the simulation clock, for its maximum time.
static void ds18b20_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct ds18b20_state *state = inst->state;

    if (state->converting && now_us >= state->converted_at_us) {
        uint32_t critical = mclw_hal_critical_enter();
        finish_conversion(state);
        mclw_hal_critical_exit(critical);
        mclw_trace(inst, MCLW_TRACE_STATE, DS18B20_SP_TEMP_LSB, 2, 0);
    }
}

// Power-up (Figure 7): the temperature register holds +85 degC, the
// reserved bytes FFh, (none given for byte 6) and 10h, and the EEPROM's
// TH, TL and configuration are recalled.
static int power_up(struct ds18b20_state *state) {
    uint8_t rom[8];

    build_rom(state->serial, rom);
    mclw_onewire_init(&state->bus, rom, ds18b20_functions,
        (uint8_t)(sizeof(ds18b20_functions) / sizeof(ds18b20_functions[0])));
    uint32_t critical = mclw_hal_critical_enter();
    memset(state->bus.memory, 0, sizeof(state->bus.memory));
    state->bus.memory[DS18B20_SP_TEMP_LSB] = 0x50;
    state->bus.memory[DS18B20_SP_TEMP_MSB] = 0x05;
    state->bus.memory[DS18B20_SP_RESERVED] = 0xff;
    state->bus.memory[DS18B20_SP_RESERVED + 1u] = 0xff;
    state->bus.memory[DS18B20_SP_RESERVED + 2u] = 0x10;
    recall(state);
    state->bus.status = true;
    state->bus.alarm = false;
    mclw_hal_critical_exit(critical);
    state->converting = false;
    state->conversions = 0;
    state->scratchpad_reads = 0;
    return mclw_hal_wire_onewire_arm(state->dq, &state->bus);
}

static void factory_eeprom(struct ds18b20_state *state) {
    state->eeprom[0] = (uint8_t)DS18B20_TH_DEFAULT;
    state->eeprom[1] = (uint8_t)DS18B20_TL_DEFAULT;
    state->eeprom[2] = DS18B20_CONFIG_DEFAULT;
}

static int ds18b20_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct ds18b20_state *state = inst->state;

    state->dq = bindings[BINDING_DQ].wire;
    state->temperature_mc = DS18B20_TEMPERATURE_DEFAULT_MC;
    state->serial = DS18B20_SERIAL_DEFAULT;
    factory_eeprom(state);
    return power_up(state);
}

static void ds18b20_deinit(struct mclw_instance *inst) {
    struct ds18b20_state *state = inst->state;

    state->dq = NULL;
}

static int ds18b20_reset(struct mclw_instance *inst, bool factory) {
    struct ds18b20_state *state = inst->state;

    if (factory) {
        state->temperature_mc = DS18B20_TEMPERATURE_DEFAULT_MC;
        factory_eeprom(state);
    }
    return power_up(state);
}

static int ds18b20_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct ds18b20_state *state = inst->state;
    uint32_t critical = mclw_hal_critical_enter();
    struct mclw_onewire_counts counts = state->bus.counts;
    uint8_t memory[DS18B20_SCRATCHPAD_BYTES];
    uint8_t rom[8];
    bool alarm = state->bus.alarm;

    memcpy(memory, state->bus.memory, sizeof(memory));
    memcpy(rom, state->bus.rom, sizeof(rom));
    mclw_hal_critical_exit(critical);
    switch (property) {
        case PROP_TEMPERATURE:
            out->v[0] = state->temperature_mc;
            return 0;
        case PROP_SERIAL:
            out->v[0] = state->serial;
            return 0;
        case PROP_ROM:
            out->v[0] = (uint32_t)rom[0] | (uint32_t)rom[1] << 8 | (uint32_t)rom[2] << 16 | (uint32_t)rom[3] << 24;
            out->v[1] = (uint32_t)rom[4] | (uint32_t)rom[5] << 8 | (uint32_t)rom[6] << 16 | (uint32_t)rom[7] << 24;
            return 0;
        case PROP_RESOLUTION:
            out->v[0] = ds18b20_resolution_bits(memory[DS18B20_SP_CONFIG]);
            return 0;
        case PROP_TH:
            out->v[0] = (int8_t)memory[DS18B20_SP_TH];
            return 0;
        case PROP_TL:
            out->v[0] = (int8_t)memory[DS18B20_SP_TL];
            return 0;
        case PROP_ALARM:
            out->v[0] = alarm ? 1 : 0;
            return 0;
        case PROP_CONVERSIONS:
            out->v[0] = state->conversions;
            return 0;
        case PROP_SCRATCHPAD_READS:
            out->v[0] = state->scratchpad_reads;
            return 0;
        case PROP_SEARCHES:
            out->v[0] = counts.searches;
            return 0;
        case PROP_MATCHES:
            out->v[0] = counts.matches;
            return 0;
        case PROP_RESETS:
            out->v[0] = counts.resets;
            return 0;
        default:
            return -EINVAL;
    }
}

static int ds18b20_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct ds18b20_state *state = inst->state;
    uint8_t rom[8];

    switch (property) {
        case PROP_TEMPERATURE:
            state->temperature_mc = (int32_t)in->v[0];
            return 0;
        case PROP_SERIAL: {
            state->serial = (uint32_t)in->v[0];
            build_rom(state->serial, rom);
            uint32_t critical = mclw_hal_critical_enter();
            memcpy(state->bus.rom, rom, sizeof(rom));
            mclw_hal_critical_exit(critical);
            return 0;
        }
        default:
            return -EINVAL;
    }
}

static int ds18b20_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    struct ds18b20_state *state = inst->state;
    const uint8_t *src = region == REGION_SCRATCHPAD ? state->bus.memory : state->bus.rom;
    size_t size = region == REGION_SCRATCHPAD ? DS18B20_SCRATCHPAD_BYTES : 8u;

    if (region > REGION_ROM || offset > size || len > size - offset) {
        return -EINVAL;
    }
    uint32_t critical = mclw_hal_critical_enter();
    memcpy(buf, &src[offset], len);
    mclw_hal_critical_exit(critical);
    return 0;
}

static const struct mclw_model_ops ds18b20_ops = {
    .init = ds18b20_init,
    .deinit = ds18b20_deinit,
    .reset = ds18b20_reset,
    .get = ds18b20_get,
    .set = ds18b20_set,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = ds18b20_read_region,
    .write_region = NULL,
    .time_advanced = ds18b20_time_advanced,
    .poll = ds18b20_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_ds18b20 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "ds18b20",
    .family = "ds18b20",
    .config = NULL,
    .properties = ds18b20_properties,
    .property_count = sizeof(ds18b20_properties) / sizeof(ds18b20_properties[0]),
    .regions = ds18b20_regions,
    .region_count = sizeof(ds18b20_regions) / sizeof(ds18b20_regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = ds18b20_bindings,
    .binding_count = sizeof(ds18b20_bindings) / sizeof(ds18b20_bindings[0]),
    .state_size = sizeof(struct ds18b20_state),
    .ops = &ds18b20_ops,
};
