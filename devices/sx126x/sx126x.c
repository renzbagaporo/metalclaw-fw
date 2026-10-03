// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/bus.h"
#include "metalclaw/command.h"
#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "sx126x.h"

enum {
    PROP_MODE,
    PROP_PACKET_TYPE,
    PROP_FREQUENCY,
    PROP_SF,
    PROP_BW,
    PROP_CR,
    PROP_LDRO,
    PROP_PREAMBLE,
    PROP_IMPLICIT_HEADER,
    PROP_PAYLOAD_LENGTH,
    PROP_CRC,
    PROP_IQ_INVERTED,
    PROP_SYNC_WORD,
    PROP_POWER,
    PROP_IRQ,
    PROP_DIO2_RF_SWITCH,
    PROP_TCXO_MV,
    PROP_COMMANDS,
    PROP_COMMAND_ERRORS,
    PROP_RESETS,
    PROP_SPI_MODE,
    PROP_NOISE_FLOOR,
    PROP_TX_PENDING,
    PROP_TX_TOTAL,
    PROP_TX_DROPPED,
    PROP_TX_LENGTH,
    PROP_TX_FREQUENCY,
    PROP_TX_PACKET_TYPE,
    PROP_TX_SF,
    PROP_TX_BW,
    PROP_TX_CR,
    PROP_TX_LDRO,
    PROP_TX_PREAMBLE,
    PROP_TX_IMPLICIT_HEADER,
    PROP_TX_CRC,
    PROP_TX_IQ_INVERTED,
    PROP_TX_SYNC_WORD,
    PROP_TX_POWER,
    PROP_TX_START_MS,
    PROP_TX_AIRTIME_US,
    PROP_TX_NEXT,
    PROP_RX_RECEIVED,
    PROP_RX_MISSED,
    PROP_RX_FREQUENCY,
    PROP_RX_SF,
    PROP_RX_BW,
    PROP_RX_CR,
    PROP_RX_PREAMBLE,
    PROP_RX_CRC,
    PROP_RX_IQ_INVERTED,
    PROP_RX_SYNC_WORD,
    PROP_RX_RSSI,
    PROP_RX_SNR,
    PROP_RX_SIGNAL_RSSI,
};

enum {
    BINDING_SPI,
    BINDING_BUSY,
    BINDING_DIO1,
    BINDING_NRESET,
};

enum {
    REGION_TX,
    REGION_RX,
};

enum {
    FAULT_CRC_ERROR,
    FAULT_HEADER_ERROR,
    FAULT_COUNT,
};

// The `mode` property's numbering.
enum sx126x_mode {
    MODE_SLEEP,
    MODE_STBY_RC,
    MODE_STBY_XOSC,
    MODE_FS,
    MODE_TX,
    MODE_RX,
    MODE_RX_DUTY_CYCLE,
    MODE_CAD,
    MODE_RESET,
};

#define TX_QUEUE 4u
#define RX_RSSI_DEFAULT (-60)
#define RX_SNR_DEFAULT 10
#define NOISE_FLOOR_DEFAULT (-120)
// Symbols a receiver needs to latch a preamble; RadioLib's duty-cycle
// arithmetic uses the same figure from Semtech.
#define PREAMBLE_DETECT_SYMBOLS 8u

struct sx126x_config {
    char version[SX126X_VERSION_LENGTH];
    uint8_t ocp_reset;
};

// One LoRa or GFSK frame's air parameters.
struct sx126x_air {
    uint32_t frequency_hz;
    uint32_t bw_hz;
    uint16_t preamble;
    uint16_t sync_word;
    uint8_t packet_type;
    uint8_t sf;
    uint8_t cr;
    bool ldro;
    bool implicit_header;
    bool crc;
    bool iq_inverted;
};

struct sx126x_tx_frame {
    struct sx126x_air air;
    uint64_t start_us;
    uint32_t airtime_us;
    int8_t power;
    uint8_t length;
    uint8_t payload[SX126X_PAYLOAD_MAX];
};

enum rx_stage {
    RX_IDLE,
    RX_PREAMBLE,
    RX_HEADER,
    RX_PAYLOAD,
};

struct sx126x_rx_frame {
    struct sx126x_air air;
    uint64_t start_us;
    uint64_t preamble_us;
    uint64_t header_us;
    uint64_t end_us;
    int16_t rssi;
    int16_t signal_rssi;
    int8_t snr;
    uint8_t fault;
    enum rx_stage stage;
    bool on_air;
    uint8_t length;
    uint8_t payload[SX126X_PAYLOAD_MAX];
};

// The next injected frame's parameters; `set` says which the script gave.
struct sx126x_injection {
    uint32_t set;
    struct sx126x_air air;
    int16_t rssi;
    int16_t signal_rssi;
    int8_t snr;
};

struct sx126x_state {
    struct mclw_command_image image;
    struct mclw_spi_target *spi;
    struct mclw_gpio *busy;
    struct mclw_gpio *dio1;
    struct mclw_gpio *nreset;
    const struct sx126x_config *config;

    enum sx126x_mode mode;
    enum sx126x_mode fallback;
    uint8_t command_status;
    bool cold_sleep;
    uint8_t packet_type;
    uint32_t rf_frequency;
    uint8_t modulation[8];
    uint8_t packet[9];
    uint8_t pa_config[4];
    int8_t tx_power;
    uint8_t tx_base;
    uint8_t rx_base;
    uint16_t irq;
    uint16_t irq_mask;
    uint16_t dio1_mask;
    bool dio2_rf_switch;
    uint16_t tcxo_mv;
    bool stop_on_preamble;
    uint8_t cad_params[7];
    uint16_t device_errors;
    uint16_t stats_received;
    uint16_t stats_crc_errors;
    uint16_t stats_header_errors;
    uint8_t rx_length;
    uint8_t rx_start;
    uint8_t rssi_packet;
    uint8_t snr_packet;
    uint8_t signal_rssi_packet;
    int16_t noise_floor;
    uint32_t random;

    bool rx_continuous;
    uint64_t rx_timeout_us;
    uint64_t tx_end_us;
    uint64_t tx_timeout_us;
    uint64_t cad_start_us;
    uint64_t cad_end_us;
    bool rx_timer_stopped;

    uint32_t commands;
    uint32_t command_errors;
    uint32_t resets;

    struct sx126x_tx_frame tx[TX_QUEUE];
    uint8_t tx_head;
    uint8_t tx_count;
    uint32_t tx_total;
    uint32_t tx_dropped;

    struct sx126x_rx_frame rx;
    struct sx126x_injection injection;
    uint32_t rx_received;
    uint32_t rx_missed;
    uint32_t faults[FAULT_COUNT];
};

#define PROP(n, t, c, a, u, lo, hi) \
    { .name = (n), .type = (t), .count = (c), .access = (a), .unit = (u), .min = (lo), .max = (hi) }
#define RO_U32(n, u, hi) PROP(n, MCLW_TYPE_U32, 1, MCLW_ACCESS_RO, u, 0, hi)
#define RO_BOOL(n) PROP(n, MCLW_TYPE_BOOL, 1, MCLW_ACCESS_RO, NULL, 0, 1)
#define RW_U32(n, u, hi) PROP(n, MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, u, 0, hi)

static const struct mclw_property_desc sx126x_properties[] = {
    RO_U32("mode", NULL, MODE_RESET),
    RO_U32("packet_type", NULL, 0xff),
    RO_U32("frequency", "Hz", UINT32_MAX),
    RO_U32("sf", NULL, 0xff),
    RO_U32("bw", "Hz", 500000),
    RO_U32("cr", NULL, 0xff),
    RO_BOOL("ldro"),
    RO_U32("preamble", "symbols", 0xffff),
    RO_BOOL("implicit_header"),
    RO_U32("payload_length", "B", 0xff),
    RO_BOOL("crc"),
    RO_BOOL("iq_inverted"),
    RO_U32("sync_word", NULL, 0xffff),
    PROP("power", MCLW_TYPE_I32, 1, MCLW_ACCESS_RO, "dBm", -128, 127),
    RO_U32("irq", NULL, 0xffff),
    RO_BOOL("dio2_rf_switch"),
    RO_U32("tcxo_mv", "mV", 3300),
    RO_U32("commands", NULL, UINT32_MAX),
    RO_U32("command_errors", NULL, UINT32_MAX),
    RO_U32("resets", NULL, UINT32_MAX),
    MCLW_PROPERTY_SPI_MODE,
    PROP("noise_floor", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "dBm", -127, 0),
    RO_U32("tx_pending", NULL, TX_QUEUE),
    RO_U32("tx_total", NULL, UINT32_MAX),
    RO_U32("tx_dropped", NULL, UINT32_MAX),
    RO_U32("tx_length", "B", SX126X_PAYLOAD_MAX),
    RO_U32("tx_frequency", "Hz", UINT32_MAX),
    RO_U32("tx_packet_type", NULL, 0xff),
    RO_U32("tx_sf", NULL, 0xff),
    RO_U32("tx_bw", "Hz", 500000),
    RO_U32("tx_cr", NULL, 0xff),
    RO_BOOL("tx_ldro"),
    RO_U32("tx_preamble", "symbols", 0xffff),
    RO_BOOL("tx_implicit_header"),
    RO_BOOL("tx_crc"),
    RO_BOOL("tx_iq_inverted"),
    RO_U32("tx_sync_word", NULL, 0xffff),
    PROP("tx_power", MCLW_TYPE_I32, 1, MCLW_ACCESS_RO, "dBm", -128, 127),
    RO_U32("tx_start_ms", "ms", UINT32_MAX),
    RO_U32("tx_airtime_us", "us", UINT32_MAX),
    PROP("tx_next", MCLW_TYPE_BOOL, 1, MCLW_ACCESS_WO, NULL, 0, 1),
    RO_U32("rx_received", NULL, UINT32_MAX),
    RO_U32("rx_missed", NULL, UINT32_MAX),
    RW_U32("rx_frequency", "Hz", 1000000000),
    PROP("rx_sf", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, NULL, 5, 12),
    RW_U32("rx_bw", "Hz", 500000),
    PROP("rx_cr", MCLW_TYPE_U32, 1, MCLW_ACCESS_RW, NULL, 1, 4),
    RW_U32("rx_preamble", "symbols", 0xffff),
    PROP("rx_crc", MCLW_TYPE_BOOL, 1, MCLW_ACCESS_RW, NULL, 0, 1),
    PROP("rx_iq_inverted", MCLW_TYPE_BOOL, 1, MCLW_ACCESS_RW, NULL, 0, 1),
    RW_U32("rx_sync_word", NULL, 0xffff),
    PROP("rx_rssi", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "dBm", -127, 0),
    PROP("rx_snr", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "dB", -32, 31),
    PROP("rx_signal_rssi", MCLW_TYPE_I32, 1, MCLW_ACCESS_RW, "dBm", -127, 0),
};

_Static_assert(sizeof(sx126x_properties) / sizeof(sx126x_properties[0]) == PROP_RX_SIGNAL_RSSI + 1,
    "property table matches the enum");

static const struct mclw_region_desc sx126x_regions[] = {
    { .name = "tx", .size = SX126X_PAYLOAD_MAX, .access = MCLW_ACCESS_RO },
    { .name = "rx", .size = SX126X_PAYLOAD_MAX, .access = MCLW_ACCESS_WO },
};

static const char *const sx126x_faults[] = { "crc_error", "header_error" };

static const struct mclw_binding_desc sx126x_bindings[] = {
    {
        .name = "spi",
        .kind = MCLW_BINDING_SPI_TARGET,
        .required = true,
        .usb = NULL,
        .spi = { .modes = MCLW_SPI_MODE_BIT(0), .default_mode = 0, .msb_first = true },
        .uart = { 0 },
    },
    { .name = "busy", .kind = MCLW_BINDING_GPIO_OUT, .required = true, .usb = NULL, .spi = { 0 }, .uart = { 0 } },
    { .name = "dio1", .kind = MCLW_BINDING_GPIO_OUT, .required = false, .usb = NULL, .spi = { 0 }, .uart = { 0 } },
    { .name = "nreset", .kind = MCLW_BINDING_GPIO_IN, .required = false, .usb = NULL, .spi = { 0 }, .uart = { 0 } },
};

// Register pages the drivers touch: 0x02 retention list, 0x03 version,
// 0x05 DIO control, 0x06 GFSK framing, 0x07 LoRa framing, 0x08 RNG and PA,
// 0x09 RTC and trims.
static const uint8_t sx126x_pages[] = { 0x02, 0x03, 0x05, 0x06, 0x07, 0x08, 0x09 };

static const uint8_t sx126x_responses[] = {
    SX126X_OP_GET_STATUS,
    SX126X_OP_GET_PACKET_TYPE,
    SX126X_OP_GET_IRQ_STATUS,
    SX126X_OP_GET_RX_BUFFER_STATUS,
    SX126X_OP_GET_PACKET_STATUS,
    SX126X_OP_GET_RSSI_INST,
    SX126X_OP_GET_STATS,
    SX126X_OP_GET_DEVICE_ERRORS,
};

static const uint32_t bandwidth_hz[16] = {
    [SX126X_BW_7] = 7812, [SX126X_BW_10] = 10417, [SX126X_BW_15] = 15625, [SX126X_BW_20] = 20833,
    [SX126X_BW_31] = 31250, [SX126X_BW_41] = 41667, [SX126X_BW_62] = 62500, [SX126X_BW_125] = 125000,
    [SX126X_BW_250] = 250000, [SX126X_BW_500] = 500000,
};

// SetDio3AsTcxoCtrl voltage codes 0..7 (section 13.3.6).
static const uint16_t tcxo_mv[8] = { 1600, 1700, 1800, 2200, 2400, 2700, 3000, 3300 };

// What each SetPaConfig/SetTxParams pair puts out, as RadioLib measured
// it (src/modules/SX126x/SX1262.cpp paOptTable, from radiolib-org/
// power-tests): entry i is i - 9 dBm. Any other pair is taken at the
// SetTxParams value, which is the datasheet's reading at full duty.
static const uint8_t pa_measured[32][3] = {
    { 2, 2, (uint8_t)-5 }, { 2, 1, 0 }, { 1, 1, 3 }, { 1, 2, 0 }, { 1, 1, 6 }, { 1, 2, 3 }, { 2, 2, 2 },
    { 4, 1, 6 }, { 1, 1, 11 }, { 2, 1, 11 }, { 1, 1, 14 }, { 2, 1, 14 }, { 1, 1, 20 }, { 1, 1, 22 },
    { 2, 2, 11 }, { 3, 1, 21 }, { 1, 2, 17 }, { 4, 2, 13 }, { 1, 2, 20 }, { 1, 2, 22 }, { 2, 2, 21 },
    { 3, 2, 21 }, { 1, 4, 19 }, { 1, 4, 20 }, { 3, 3, 20 }, { 2, 5, 19 }, { 1, 6, 22 }, { 2, 5, 22 },
    { 3, 5, 22 }, { 3, 6, 22 }, { 4, 6, 22 }, { 4, 7, 22 },
};

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint64_t steps_to_us(uint32_t steps) {
    return ((uint64_t)steps * SX126X_TIMEOUT_STEP_NUM + SX126X_TIMEOUT_STEP_DEN - 1u) / SX126X_TIMEOUT_STEP_DEN;
}

static uint32_t frequency_hz(uint32_t rf) {
    return (uint32_t)(((uint64_t)rf * SX126X_FREQ_STEP_NUM + (1u << (SX126X_FREQ_STEP_SHIFT - 1u))) >>
        SX126X_FREQ_STEP_SHIFT);
}

static int8_t output_power(const struct sx126x_state *state) {
    if (state->pa_config[2] == 0u) {
        for (size_t i = 0; i < sizeof(pa_measured) / sizeof(pa_measured[0]); i++) {
            if (pa_measured[i][0] == state->pa_config[0] && pa_measured[i][1] == state->pa_config[1] &&
                (int8_t)pa_measured[i][2] == state->tx_power) {
                return (int8_t)((int)i - 9);
            }
        }
    }
    return state->tx_power;
}

static uint16_t sync_word(const struct sx126x_state *state) {
    return (uint16_t)((mclw_command_get_register(&state->image, SX126X_REG_LORA_SYNC_WORD) << 8) |
        mclw_command_get_register(&state->image, SX126X_REG_LORA_SYNC_WORD + 1u));
}

// The radio's current settings, read the LoRa way.
static struct sx126x_air radio_air(const struct sx126x_state *state) {
    struct sx126x_air air = {
        .frequency_hz = frequency_hz(state->rf_frequency),
        .bw_hz = bandwidth_hz[state->modulation[1] & 0x0fu],
        .preamble = be16(&state->packet[0]),
        .sync_word = sync_word(state),
        .packet_type = state->packet_type,
        .sf = state->modulation[0],
        .cr = state->modulation[2],
        .ldro = (state->modulation[3] & 1u) != 0u,
        .implicit_header = state->packet[2] != 0u,
        .crc = state->packet[4] != 0u,
        .iq_inverted = state->packet[5] != 0u,
    };
    return air;
}

static uint32_t symbol_us(const struct sx126x_air *air) {
    return air->bw_hz == 0u ? 0u : (uint32_t)(((uint64_t)1000000u << air->sf) / air->bw_hz);
}

// Section 6.1.4, written the way RadioLib's getTimeOnAir rounds it.
static uint32_t lora_airtime_us(const struct sx126x_air *air, uint8_t length) {
    uint32_t sf = air->sf;
    uint32_t preamble_quarters = sf < 7u ? 25u : 17u;
    int32_t bits = 8 * (int32_t)length + (air->crc ? 16 : 0) - 4 * (int32_t)sf + (sf < 7u ? 0 : 8) +
        (air->implicit_header ? 0 : 20);
    uint32_t divisor = 4u * (air->ldro && sf > 2u ? sf - 2u : sf);
    uint32_t blocks;
    uint32_t denominator = air->cr <= 4u ? air->cr + 4u : (air->cr == 7u ? 8u : air->cr);
    uint64_t quarters;

    if (bits < 0) {
        bits = 0;
    }
    if (divisor == 0u || air->bw_hz == 0u || sf > 12u) {
        return 0;
    }
    blocks = ((uint32_t)bits + divisor - 1u) / divisor;
    quarters = ((uint64_t)air->preamble + 8u) * 4u + preamble_quarters + (uint64_t)blocks * denominator * 4u;
    return (uint32_t)((quarters * ((uint64_t)1000000u << sf)) / (4u * (uint64_t)air->bw_hz));
}

// GFSK framing (section 6.2.3): preamble, sync word, an optional length
// byte, payload and CRC, at the programmed bit rate.
static uint32_t gfsk_airtime_us(const struct sx126x_state *state, uint8_t length) {
    uint32_t br = be24(&state->modulation[0]);
    uint32_t bits = be16(&state->packet[0]) + state->packet[3] + 8u * length;
    uint8_t crc = state->packet[7];

    if (state->packet[5] != 0u) {
        bits += 8u;
    }
    if (crc == 0x00u || crc == 0x04u) {
        bits += 8u;
    } else if (crc == 0x02u || crc == 0x06u) {
        bits += 16u;
    }
    // bit rate = 32 * Fxtal / br, Fxtal 32 MHz.
    return br == 0u ? 0u : (uint32_t)(((uint64_t)bits * br) / 1024u);
}

static uint8_t mode_bits(enum sx126x_mode mode) {
    switch (mode) {
        case MODE_STBY_XOSC:
            return SX126X_STATUS_MODE_STBY_XOSC;
        case MODE_FS:
            return SX126X_STATUS_MODE_FS;
        case MODE_TX:
            return SX126X_STATUS_MODE_TX;
        case MODE_RX:
        case MODE_RX_DUTY_CYCLE:
        case MODE_CAD:
            return SX126X_STATUS_MODE_RX;
        case MODE_SLEEP:
        case MODE_STBY_RC:
        case MODE_RESET:
        default:
            return SX126X_STATUS_MODE_STBY_RC;
    }
}

static uint8_t status_byte(const struct sx126x_state *state) {
    return (uint8_t)(SX126X_STATUS_RESERVED | (mode_bits(state->mode) << 4) | (state->command_status << 1));
}

static uint8_t rssi_byte(int16_t dbm) {
    int32_t v = -2 * (int32_t)dbm;

    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static bool on_air_now(const struct sx126x_state *state, uint64_t now) {
    return state->rx.on_air && now >= state->rx.start_us && now < state->rx.end_us;
}

static void set_answers(struct sx126x_state *state) {
    struct mclw_command_image *image = &state->image;
    uint8_t status = status_byte(state);
    uint8_t get_status = (uint8_t)(status & ~SX126X_STATUS_RESERVED);
    uint8_t rssi_inst = rssi_byte(on_air_now(state, mclw_now_us()) ? state->rx.rssi : state->noise_floor);
    uint8_t bytes[6];

    mclw_command_set_status(image, status);
    mclw_command_set_response(image, SX126X_OP_GET_STATUS, &get_status, 1);
    mclw_command_set_response(image, SX126X_OP_GET_PACKET_TYPE, &state->packet_type, 1);
    bytes[0] = (uint8_t)(state->irq >> 8);
    bytes[1] = (uint8_t)state->irq;
    mclw_command_set_response(image, SX126X_OP_GET_IRQ_STATUS, bytes, 2);
    bytes[0] = state->rx_length;
    bytes[1] = state->rx_start;
    mclw_command_set_response(image, SX126X_OP_GET_RX_BUFFER_STATUS, bytes, 2);
    bytes[0] = state->rssi_packet;
    bytes[1] = state->snr_packet;
    bytes[2] = state->signal_rssi_packet;
    mclw_command_set_response(image, SX126X_OP_GET_PACKET_STATUS, bytes, 3);
    mclw_command_set_response(image, SX126X_OP_GET_RSSI_INST, &rssi_inst, 1);
    bytes[0] = (uint8_t)(state->stats_received >> 8);
    bytes[1] = (uint8_t)state->stats_received;
    bytes[2] = (uint8_t)(state->stats_crc_errors >> 8);
    bytes[3] = (uint8_t)state->stats_crc_errors;
    bytes[4] = (uint8_t)(state->stats_header_errors >> 8);
    bytes[5] = (uint8_t)state->stats_header_errors;
    mclw_command_set_response(image, SX126X_OP_GET_STATS, bytes, 6);
    bytes[0] = (uint8_t)(state->device_errors >> 8);
    bytes[1] = (uint8_t)state->device_errors;
    mclw_command_set_response(image, SX126X_OP_GET_DEVICE_ERRORS, bytes, 2);
}

static void set_absent(struct mclw_command_image *image) {
    uint8_t bytes[MCLW_COMMAND_RESPONSE_MAX];

    memset(bytes, SX126X_STATUS_ABSENT, sizeof(bytes));
    mclw_command_set_status(image, SX126X_STATUS_ABSENT);
    for (size_t i = 0; i < sizeof(sx126x_responses); i++) {
        mclw_command_set_response(image, sx126x_responses[i], bytes, sizeof(bytes));
    }
}

// Rebuilds everything a read may serve and hands it to the engine.
static void publish(struct sx126x_state *state) {
    if (state->mode == MODE_SLEEP || state->mode == MODE_RESET) {
        set_absent(&state->image);
    } else {
        set_answers(state);
    }
    mclw_command_publish(&state->image);
    if (state->dio1 != NULL) {
        mclw_hal_gpio_write(state->dio1, (state->irq & state->dio1_mask) != 0u);
    }
}

static void raise_irq(struct sx126x_state *state, uint16_t bits) {
    uint16_t raised = (uint16_t)(bits & state->irq_mask & ~state->irq);

    state->irq |= raised;
    if (raised != 0u) {
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_STATE, raised, 0, 0);
    }
}

static void next_random(struct sx126x_state *state) {
    uint8_t bytes[SX126X_RANDOM_LENGTH];

    // xorshift32, seeded per attach; the drivers only need it to move.
    state->random ^= state->random << 13;
    state->random ^= state->random >> 17;
    state->random ^= state->random << 5;
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (uint8_t)(state->random >> (8u * i));
    }
    mclw_command_set_registers(&state->image, SX126X_REG_RANDOM, bytes, sizeof(bytes));
}

static void set_register(struct sx126x_state *state, uint16_t address, uint8_t value) {
    mclw_command_set_registers(&state->image, address, &value, 1);
}

// A frame the radio stops receiving is missed, as one it never heard.
static void drop_rx(struct sx126x_state *state) {
    if (state->rx.stage != RX_IDLE) {
        state->rx.stage = RX_IDLE;
        state->rx_missed++;
    }
}

// Power-on and cold-start state: section 12 reset values.
static void power_on(struct sx126x_state *state) {
    static const uint8_t gfsk_sync[8] = { 0x97, 0x23, 0x52, 0x25, 0x56, 0x53, 0x65, 0x64 };
    struct mclw_command_image *image = &state->image;

    for (uint8_t i = 0; i < image->page_count; i++) {
        memset(image->pages[i], 0, sizeof(image->pages[i]));
    }
    memset(image->buffer, 0, sizeof(image->buffer));
    image->dirty |= MCLW_COMMAND_DIRTY_PAGES | MCLW_COMMAND_DIRTY_BUFFER;
    mclw_command_set_registers(image, SX126X_REG_VERSION, (const uint8_t *)state->config->version,
        SX126X_VERSION_LENGTH);
    set_register(state, SX126X_REG_WHITENING, 0x01);
    set_register(state, SX126X_REG_CRC_INIT, 0x1D);
    set_register(state, SX126X_REG_CRC_INIT + 1u, 0x0F);
    set_register(state, SX126X_REG_CRC_POLY, 0x10);
    set_register(state, SX126X_REG_CRC_POLY + 1u, 0x21);
    mclw_command_set_registers(image, SX126X_REG_GFSK_SYNC_WORD, gfsk_sync, sizeof(gfsk_sync));
    set_register(state, SX126X_REG_IQ_POLARITY, SX126X_IQ_POLARITY_RESET);
    set_register(state, SX126X_REG_LORA_SYNC_WORD, (uint8_t)(SX126X_LORA_SYNC_WORD_PRIVATE >> 8));
    set_register(state, SX126X_REG_LORA_SYNC_WORD + 1u, (uint8_t)SX126X_LORA_SYNC_WORD_PRIVATE);
    set_register(state, SX126X_REG_TX_MODULATION, SX126X_TX_MODULATION_RESET);
    set_register(state, SX126X_REG_RX_GAIN, SX126X_RX_GAIN_RESET);
    set_register(state, SX126X_REG_TX_CLAMP, SX126X_TX_CLAMP_RESET);
    set_register(state, SX126X_REG_OCP, state->config->ocp_reset);
    set_register(state, SX126X_REG_XTA_TRIM, SX126X_XTAL_TRIM_RESET);
    set_register(state, SX126X_REG_XTB_TRIM, SX126X_XTAL_TRIM_RESET);
    next_random(state);

    state->mode = MODE_STBY_RC;
    state->fallback = MODE_STBY_RC;
    // A real SX1262 out of reset reports "failure to execute" until its first
    // command; RadioLib's begin() retries SetStandby on it.
    state->command_status = SX126X_STATUS_CMD_EXEC_FAILURE;
    state->packet_type = SX126X_PACKET_TYPE_GFSK;
    state->rf_frequency = 0;
    memset(state->modulation, 0, sizeof(state->modulation));
    state->modulation[0] = 7;
    state->modulation[1] = SX126X_BW_125;
    state->modulation[2] = 1;
    memset(state->packet, 0, sizeof(state->packet));
    state->packet[1] = 8;
    state->packet[3] = SX126X_PAYLOAD_MAX;
    state->packet[4] = 1;
    memset(state->pa_config, 0, sizeof(state->pa_config));
    state->tx_power = 0;
    state->tx_base = 0;
    state->rx_base = 0;
    state->irq = 0;
    state->irq_mask = 0;
    state->dio1_mask = 0;
    state->dio2_rf_switch = false;
    state->tcxo_mv = 0;
    state->stop_on_preamble = false;
    memset(state->cad_params, 0, sizeof(state->cad_params));
    // The modules this models run from a TCXO that DIO3 powers, and nothing
    // powers it yet: the power-on calibration cannot start the oscillator.
    // A real SX1262 module reports this at its first GetDeviceErrors.
    state->device_errors = SX126X_DEVICE_ERROR_XOSC_START;
    state->stats_received = 0;
    state->stats_crc_errors = 0;
    state->stats_header_errors = 0;
    state->rx_length = 0;
    state->rx_start = 0;
    state->rssi_packet = 0;
    state->snr_packet = 0;
    state->signal_rssi_packet = 0;
    state->rx_continuous = false;
    state->rx_timeout_us = 0;
    state->tx_end_us = 0;
    state->tx_timeout_us = 0;
    state->cad_end_us = 0;
    drop_rx(state);
}

static void set_busy(struct sx126x_state *state, bool busy) {
    mclw_hal_spi_target_set_busy(state->spi, busy);
}

// Leaves whatever the radio was doing for `mode`. A frame being received
// is lost.
static void enter(struct sx126x_state *state, enum sx126x_mode mode) {
    state->mode = mode;
    state->tx_end_us = 0;
    state->tx_timeout_us = 0;
    state->rx_timeout_us = 0;
    state->cad_end_us = 0;
    if (mode != MODE_RX && mode != MODE_RX_DUTY_CYCLE) {
        drop_rx(state);
    }
}

static void capture_tx(struct sx126x_state *state, uint64_t now, uint32_t timeout_steps) {
    struct sx126x_tx_frame *f;
    struct sx126x_air air = radio_air(state);
    uint8_t length = state->packet_type == SX126X_PACKET_TYPE_LORA ? state->packet[3] : state->packet[6];
    uint32_t airtime = state->packet_type == SX126X_PACKET_TYPE_LORA ? lora_airtime_us(&air, length) :
        gfsk_airtime_us(state, length);

    enter(state, MODE_TX);
    state->tx_end_us = now + airtime;
    state->tx_timeout_us = timeout_steps == 0u ? 0u : now + steps_to_us(timeout_steps);
    state->tx_total++;
    if (state->tx_count >= TX_QUEUE) {
        state->tx_dropped++;
        return;
    }
    f = &state->tx[(state->tx_head + state->tx_count) % TX_QUEUE];
    state->tx_count++;
    f->air = air;
    f->start_us = now;
    f->airtime_us = airtime;
    f->power = output_power(state);
    f->length = length;
    mclw_command_read_buffer(&state->image, state->tx_base, f->payload, length);
}

static void start_rx(struct sx126x_state *state, uint64_t now, uint32_t timeout_steps) {
    enter(state, MODE_RX);
    state->rx_continuous = timeout_steps == SX126X_RX_CONTINUOUS;
    state->rx_timer_stopped = false;
    if (!state->rx_continuous && timeout_steps != 0u) {
        state->rx_timeout_us = now + steps_to_us(timeout_steps);
    }
}

static uint32_t cad_symbols(uint8_t code) {
    return code <= 4u ? 1u << code : 1u;
}

// Whether the radio, listening now, would demodulate a frame with `air`.
static bool receivable(const struct sx126x_state *state, const struct sx126x_air *air) {
    struct sx126x_air radio = radio_air(state);
    uint32_t offset = air->frequency_hz > radio.frequency_hz ? air->frequency_hz - radio.frequency_hz :
        radio.frequency_hz - air->frequency_hz;

    return (state->mode == MODE_RX || state->mode == MODE_RX_DUTY_CYCLE) &&
        state->packet_type == SX126X_PACKET_TYPE_LORA && air->sf == radio.sf && air->bw_hz == radio.bw_hz &&
        offset <= radio.bw_hz / 4u && air->sync_word == radio.sync_word && air->iq_inverted == radio.iq_inverted;
}

static void rx_done(struct sx126x_state *state) {
    struct sx126x_rx_frame *f = &state->rx;
    uint16_t bits = SX126X_IRQ_RX_DONE;

    mclw_command_write_buffer(&state->image, state->rx_base, f->payload, f->length);
    state->rx_length = f->length;
    state->rx_start = state->rx_base;
    state->rssi_packet = rssi_byte(f->rssi);
    state->snr_packet = (uint8_t)(int8_t)(f->snr * 4);
    state->signal_rssi_packet = rssi_byte(f->signal_rssi);
    if (f->fault == FAULT_CRC_ERROR + 1u) {
        bits |= SX126X_IRQ_CRC_ERR;
        state->stats_crc_errors++;
    } else {
        state->stats_received++;
    }
    raise_irq(state, bits);
    state->command_status = SX126X_STATUS_CMD_DATA_AVAILABLE;
    state->rx_received++;
    f->stage = RX_IDLE;
    if (!state->rx_continuous || state->mode == MODE_RX_DUTY_CYCLE) {
        enter(state, state->fallback);
    }
}

// Whether a single RX's timer runs out by `at`.
static bool rx_expired(const struct sx126x_state *state, uint64_t at) {
    return state->mode == MODE_RX && state->rx_timeout_us != 0u && !state->rx_timer_stopped &&
        at >= state->rx_timeout_us;
}

// Everything due by `now`, in the order the radio would raise it. A late
// tick sees several events at once; a frame event after the RX timeout
// never happens.
static bool advance(struct sx126x_state *state, uint64_t now) {
    struct sx126x_rx_frame *f = &state->rx;
    uint16_t irq = state->irq;
    enum sx126x_mode mode = state->mode;
    bool on_air = f->on_air;

    if (f->stage == RX_PREAMBLE && now >= f->preamble_us && !rx_expired(state, f->preamble_us)) {
        raise_irq(state, SX126X_IRQ_PREAMBLE_DETECTED);
        if (state->stop_on_preamble) {
            state->rx_timer_stopped = true;
        }
        f->stage = RX_HEADER;
    }
    if (f->stage == RX_HEADER && now >= f->header_us && !rx_expired(state, f->header_us)) {
        if (f->fault == FAULT_HEADER_ERROR + 1u) {
            raise_irq(state, SX126X_IRQ_HEADER_ERR);
            state->stats_header_errors++;
            f->stage = RX_IDLE;
        } else {
            if (!f->air.implicit_header) {
                raise_irq(state, SX126X_IRQ_HEADER_VALID);
            }
            state->rx_timer_stopped = true;
            f->stage = RX_PAYLOAD;
        }
    }
    if (f->stage == RX_PAYLOAD && now >= f->end_us) {
        rx_done(state);
    }
    if (f->on_air && now >= f->end_us) {
        f->on_air = false;
    }
    if (rx_expired(state, now)) {
        raise_irq(state, SX126X_IRQ_TIMEOUT);
        enter(state, state->fallback);
    }
    if (state->mode == MODE_TX && state->tx_timeout_us != 0u && now >= state->tx_timeout_us &&
        (state->tx_end_us == 0u || state->tx_timeout_us < state->tx_end_us)) {
        raise_irq(state, SX126X_IRQ_TIMEOUT);
        enter(state, state->fallback);
    }
    if (state->mode == MODE_TX && state->tx_end_us != 0u && now >= state->tx_end_us) {
        raise_irq(state, SX126X_IRQ_TX_DONE);
        state->command_status = SX126X_STATUS_CMD_TX_DONE;
        enter(state, state->fallback);
    }
    if (state->mode == MODE_CAD && now >= state->cad_end_us) {
        struct sx126x_air radio = radio_air(state);
        bool detected = f->end_us > state->cad_start_us && f->start_us < state->cad_end_us &&
            f->air.sf == radio.sf && f->air.bw_hz == radio.bw_hz &&
            (f->air.frequency_hz > radio.frequency_hz ? f->air.frequency_hz - radio.frequency_hz :
                radio.frequency_hz - f->air.frequency_hz) <= radio.bw_hz / 4u;

        raise_irq(state, (uint16_t)(SX126X_IRQ_CAD_DONE | (detected ? SX126X_IRQ_CAD_DETECTED : 0u)));
        if (detected && state->cad_params[3] == 0x01u) {
            start_rx(state, now, be24(&state->cad_params[4]));
        } else {
            enter(state, MODE_STBY_RC);
        }
    }
    return irq != state->irq || mode != state->mode || on_air != f->on_air;
}

// On a real SX1262 "data available" and "TX done" survive every read and
// ClearIrqStatus, and the next command that does something replaces them.
// An error is kept the same way.
static bool keeps_status(uint8_t op) {
    switch (op) {
        case SX126X_OP_CLEAR_IRQ_STATUS:
        case SX126X_OP_GET_STATS:
        case SX126X_OP_GET_PACKET_TYPE:
        case SX126X_OP_GET_IRQ_STATUS:
        case SX126X_OP_GET_RX_BUFFER_STATUS:
        case SX126X_OP_GET_PACKET_STATUS:
        case SX126X_OP_GET_RSSI_INST:
        case SX126X_OP_GET_DEVICE_ERRORS:
        case SX126X_OP_READ_REGISTER:
        case SX126X_OP_READ_BUFFER:
        case SX126X_OP_GET_STATUS:
            return true;
        default:
            return false;
    }
}

static bool params(size_t have, size_t need) {
    return have >= need;
}

// One command, as the host sent it. False for a command the part would
// refuse: an unknown opcode, too few parameters, a value out of range.
static bool execute(struct sx126x_state *state, uint8_t op, const uint8_t *p, size_t n, uint64_t now) {
    switch (op) {
        case SX126X_OP_RESET_STATS:
            state->stats_received = 0;
            state->stats_crc_errors = 0;
            state->stats_header_errors = 0;
            return true;
        case SX126X_OP_GET_STATUS:
        case SX126X_OP_GET_PACKET_TYPE:
        case SX126X_OP_GET_IRQ_STATUS:
        case SX126X_OP_GET_RX_BUFFER_STATUS:
        case SX126X_OP_GET_PACKET_STATUS:
        case SX126X_OP_GET_RSSI_INST:
        case SX126X_OP_GET_STATS:
        case SX126X_OP_GET_DEVICE_ERRORS:
        case SX126X_OP_READ_BUFFER:
            return true;
        case SX126X_OP_READ_REGISTER:
            if (n >= 2u) {
                uint16_t address = be16(p);

                if (address + SX126X_RANDOM_LENGTH > SX126X_REG_RANDOM &&
                    address < SX126X_REG_RANDOM + SX126X_RANDOM_LENGTH) {
                    next_random(state);
                }
            }
            return true;
        case SX126X_OP_WRITE_REGISTER: {
            uint16_t address;

            if (!params(n, 3)) {
                return false;
            }
            address = be16(p);
            for (size_t i = 2; i < n; i++) {
                uint16_t a = (uint16_t)(address + i - 2u);

                if ((a >= SX126X_REG_VERSION && a < SX126X_REG_VERSION + SX126X_VERSION_LENGTH) ||
                    (a >= SX126X_REG_RANDOM && a < SX126X_REG_RANDOM + SX126X_RANDOM_LENGTH)) {
                    continue;
                }
                set_register(state, a, p[i]);
            }
            return true;
        }
        case SX126X_OP_WRITE_BUFFER:
            if (!params(n, 1)) {
                return false;
            }
            mclw_command_write_buffer(&state->image, p[0], p + 1, n - 1u);
            return true;
        case SX126X_OP_CLEAR_IRQ_STATUS:
            if (!params(n, 2)) {
                return false;
            }
            state->irq &= (uint16_t)~be16(p);
            return true;
        case SX126X_OP_CLEAR_DEVICE_ERRORS:
            state->device_errors = 0;
            return true;
        case SX126X_OP_SET_DIO_IRQ_PARAMS:
            if (!params(n, 4)) {
                return false;
            }
            state->irq_mask = be16(p);
            state->dio1_mask = be16(p + 2);
            return true;
        case SX126X_OP_SET_STANDBY:
            if (!params(n, 1) || p[0] > 1u) {
                return false;
            }
            enter(state, p[0] == 0u ? MODE_STBY_RC : MODE_STBY_XOSC);
            return true;
        case SX126X_OP_SET_SLEEP:
            if (!params(n, 1)) {
                return false;
            }
            enter(state, MODE_SLEEP);
            state->cold_sleep = (p[0] & 0x04u) == 0u;
            return true;
        case SX126X_OP_SET_FS:
            enter(state, MODE_FS);
            return true;
        case SX126X_OP_SET_TX:
            if (!params(n, 3)) {
                return false;
            }
            capture_tx(state, now, be24(p));
            return true;
        case SX126X_OP_SET_RX:
            if (!params(n, 3)) {
                return false;
            }
            start_rx(state, now, be24(p));
            return true;
        case SX126X_OP_SET_RX_DUTY_CYCLE:
            if (!params(n, 6)) {
                return false;
            }
            enter(state, MODE_RX_DUTY_CYCLE);
            state->rx_continuous = false;
            return true;
        case SX126X_OP_SET_CAD_PARAMS:
            if (!params(n, 7)) {
                return false;
            }
            memcpy(state->cad_params, p, sizeof(state->cad_params));
            return true;
        case SX126X_OP_SET_CAD: {
            struct sx126x_air radio = radio_air(state);

            if (state->packet_type != SX126X_PACKET_TYPE_LORA) {
                return false;
            }
            enter(state, MODE_CAD);
            state->cad_start_us = now;
            state->cad_end_us = now + (uint64_t)symbol_us(&radio) * cad_symbols(state->cad_params[0]);
            return true;
        }
        case SX126X_OP_SET_TX_CONTINUOUS_WAVE:
        case SX126X_OP_SET_TX_INFINITE_PREAMBLE:
            enter(state, MODE_TX);
            return true;
        case SX126X_OP_SET_RF_FREQUENCY:
            if (!params(n, 4)) {
                return false;
            }
            state->rf_frequency = ((uint32_t)p[0] << 24) | be24(p + 1);
            return true;
        case SX126X_OP_SET_PACKET_TYPE:
            if (!params(n, 1) || p[0] > SX126X_PACKET_TYPE_LORA) {
                return false;
            }
            state->packet_type = p[0];
            return true;
        case SX126X_OP_SET_MODULATION_PARAMS:
            if (!params(n, 4)) {
                return false;
            }
            if (state->packet_type == SX126X_PACKET_TYPE_LORA &&
                (p[0] < 5u || p[0] > 12u || p[1] > 0x0Au || bandwidth_hz[p[1]] == 0u || p[2] < 1u ||
                    p[2] > 7u || p[3] > 1u)) {
                return false;
            }
            memset(state->modulation, 0, sizeof(state->modulation));
            memcpy(state->modulation, p, n > sizeof(state->modulation) ? sizeof(state->modulation) : n);
            return true;
        case SX126X_OP_SET_PACKET_PARAMS:
            if (!params(n, 6)) {
                return false;
            }
            memcpy(state->packet, p, n > sizeof(state->packet) ? sizeof(state->packet) : n);
            return true;
        case SX126X_OP_SET_PA_CONFIG:
            if (!params(n, 4)) {
                return false;
            }
            memcpy(state->pa_config, p, sizeof(state->pa_config));
            set_register(state, SX126X_REG_OCP, p[2] == 0u ? SX126X_OCP_SX1262 : SX126X_OCP_SX1261);
            return true;
        case SX126X_OP_SET_TX_PARAMS:
            if (!params(n, 2)) {
                return false;
            }
            state->tx_power = (int8_t)p[0];
            return true;
        case SX126X_OP_SET_BUFFER_BASE_ADDRESS:
            if (!params(n, 2)) {
                return false;
            }
            state->tx_base = p[0];
            state->rx_base = p[1];
            return true;
        case SX126X_OP_SET_RX_TX_FALLBACK_MODE:
            if (!params(n, 1)) {
                return false;
            }
            state->fallback = p[0] == 0x40u ? MODE_FS : (p[0] == 0x30u ? MODE_STBY_XOSC : MODE_STBY_RC);
            return true;
        case SX126X_OP_SET_REGULATOR_MODE:
        case SX126X_OP_CALIBRATE:
        case SX126X_OP_SET_LORA_SYMB_NUM_TIMEOUT:
            return params(n, 1);
        case SX126X_OP_CALIBRATE_IMAGE:
            return params(n, 2);
        case SX126X_OP_SET_DIO2_AS_RF_SWITCH_CTRL:
            if (!params(n, 1)) {
                return false;
            }
            state->dio2_rf_switch = (p[0] & 1u) != 0u;
            return true;
        case SX126X_OP_SET_DIO3_AS_TCXO_CTRL:
            if (!params(n, 4) || p[0] > 7u) {
                return false;
            }
            state->tcxo_mv = tcxo_mv[p[0]];
            set_register(state, SX126X_REG_XTA_TRIM, SX126X_XTAL_TRIM_TCXO);
            set_register(state, SX126X_REG_XTB_TRIM, SX126X_XTAL_TRIM_TCXO);
            return true;
        case SX126X_OP_STOP_TIMER_ON_PREAMBLE:
            if (!params(n, 1)) {
                return false;
            }
            state->stop_on_preamble = p[0] != 0u;
            return true;
        default:
            return false;
    }
}

// Called on each tick and before each window, so a pulse shorter than a
// tick, or one while the clock is paused, still resets the part before the
// host's next command.
static void check_reset(struct sx126x_state *state) {
    bool fell;
    bool low;

    if (state->nreset == NULL) {
        return;
    }
    fell = mclw_hal_gpio_fell(state->nreset);
    low = !mclw_hal_gpio_read(state->nreset);
    if (fell || (low && state->mode != MODE_RESET)) {
        set_busy(state, true);
        power_on(state);
        state->mode = MODE_RESET;
        state->resets++;
        if (low) {
            publish(state);
        }
    }
    if (!low && state->mode == MODE_RESET) {
        state->mode = MODE_STBY_RC;
        publish(state);
        set_busy(state, false);
    }
}

// Worker context, runtime lock held, after BUSY went high.
static void on_window(struct mclw_command_image *image, const uint8_t *bytes, size_t len, bool busy,
    void *ctx) {
    struct sx126x_state *state = ctx;
    uint64_t now = mclw_now_us();

    (void)image;
    check_reset(state);
    if (state->mode == MODE_RESET) {
        return;
    }
    if (state->mode == MODE_SLEEP) {
        // Any chip-select falling edge wakes the part; the window itself is
        // lost (section 13.1.1).
        if (state->cold_sleep) {
            power_on(state);
        }
        state->mode = MODE_STBY_RC;
    } else if (!busy && len > 0u &&
        !(bytes[0] == SX126X_OP_RESET_STATS && len < 1u + SX126X_RESET_STATS_PARAMS)) {
        bool ok = execute(state, bytes[0], bytes + 1, len - 1u, now);

        state->commands++;
        if (!ok) {
            state->command_errors++;
            state->command_status = SX126X_STATUS_CMD_PROCESSING_ERROR;
        } else if (!keeps_status(bytes[0])) {
            state->command_status = SX126X_STATUS_CMD_OK;
        }
        mclw_trace(mclw_instance_from_state(state), MCLW_TRACE_COMMAND, bytes[0], (uint16_t)len,
            ok ? 0 : -EINVAL);
        (void)advance(state, now);
    }
    publish(state);
    if (state->mode != MODE_SLEEP) {
        set_busy(state, false);
    }
}

static void sx126x_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct sx126x_state *state = inst->state;

    check_reset(state);
    if (state->mode != MODE_RESET && advance(state, now_us)) {
        publish(state);
    }
}

static int sx126x_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct sx126x_state *state = inst->state;
    int err = 0;

    state->spi = bindings[BINDING_SPI].spi;
    state->busy = bindings[BINDING_BUSY].gpio;
    state->dio1 = bindings[BINDING_DIO1].present ? bindings[BINDING_DIO1].gpio : NULL;
    state->nreset = bindings[BINDING_NRESET].present ? bindings[BINDING_NRESET].gpio : NULL;
    state->config = inst->model->config;
    state->noise_floor = NOISE_FLOOR_DEFAULT;
    state->random = 0x2545F491u ^ (uint32_t)mclw_now_us();
    mclw_command_init(&state->image, SX126X_OP_READ_REGISTER, SX126X_OP_READ_BUFFER);
    mclw_command_set_on_window(&state->image, on_window, state);
    for (size_t i = 0; err == 0 && i < sizeof(sx126x_pages); i++) {
        err = mclw_command_add_page(&state->image, sx126x_pages[i]);
    }
    for (size_t i = 0; err == 0 && i < sizeof(sx126x_responses); i++) {
        err = mclw_command_add_response(&state->image, sx126x_responses[i]);
    }
    if (err != 0) {
        return err;
    }
    power_on(state);
    publish(state);
    err = mclw_hal_spi_target_arm_command(state->spi, &state->image, state->busy);
    if (err != 0) {
        return err;
    }
    state->mode = MODE_STBY_RC;
    check_reset(state);
    if (state->mode != MODE_RESET) {
        set_busy(state, false);
    }
    return 0;
}

static void sx126x_deinit(struct mclw_instance *inst) {
    struct sx126x_state *state = inst->state;

    mclw_hal_spi_target_disarm(state->spi);
    state->spi = NULL;
}

static int sx126x_reset(struct mclw_instance *inst, bool factory) {
    struct sx126x_state *state = inst->state;

    power_on(state);
    if (state->nreset != NULL && !mclw_hal_gpio_read(state->nreset)) {
        state->mode = MODE_RESET;
    }
    state->tx_count = 0;
    state->rx.on_air = false;
    memset(&state->injection, 0, sizeof(state->injection));
    if (factory) {
        state->noise_floor = NOISE_FLOOR_DEFAULT;
        memset(state->faults, 0, sizeof(state->faults));
    }
    publish(state);
    set_busy(state, state->mode == MODE_RESET);
    return 0;
}

static const struct sx126x_tx_frame *front(const struct sx126x_state *state) {
    return state->tx_count == 0u ? NULL : &state->tx[state->tx_head];
}

static int tx_get(const struct sx126x_state *state, size_t property, int64_t *out) {
    const struct sx126x_tx_frame *f = front(state);

    if (f == NULL) {
        return -ENOENT;
    }
    switch (property) {
        case PROP_TX_LENGTH: *out = f->length; break;
        case PROP_TX_FREQUENCY: *out = f->air.frequency_hz; break;
        case PROP_TX_PACKET_TYPE: *out = f->air.packet_type; break;
        case PROP_TX_SF: *out = f->air.sf; break;
        case PROP_TX_BW: *out = f->air.bw_hz; break;
        case PROP_TX_CR: *out = f->air.cr; break;
        case PROP_TX_LDRO: *out = f->air.ldro; break;
        case PROP_TX_PREAMBLE: *out = f->air.preamble; break;
        case PROP_TX_IMPLICIT_HEADER: *out = f->air.implicit_header; break;
        case PROP_TX_CRC: *out = f->air.crc; break;
        case PROP_TX_IQ_INVERTED: *out = f->air.iq_inverted; break;
        case PROP_TX_SYNC_WORD: *out = f->air.sync_word; break;
        case PROP_TX_POWER: *out = f->power; break;
        case PROP_TX_START_MS: *out = (uint32_t)(f->start_us / 1000u); break;
        case PROP_TX_AIRTIME_US: *out = f->airtime_us; break;
        default: return -EINVAL;
    }
    return 0;
}

// An injection parameter the script did not give is the radio's own.
static struct sx126x_air injection_air(const struct sx126x_state *state) {
    struct sx126x_air air = radio_air(state);
    const struct sx126x_injection *in = &state->injection;

    air.packet_type = SX126X_PACKET_TYPE_LORA;
    air.implicit_header = false;
#define TAKE(prop, field) \
    if ((in->set & (1u << ((prop) - PROP_RX_FREQUENCY))) != 0u) { \
        air.field = in->air.field; \
    }
    TAKE(PROP_RX_FREQUENCY, frequency_hz)
    TAKE(PROP_RX_SF, sf)
    TAKE(PROP_RX_BW, bw_hz)
    TAKE(PROP_RX_CR, cr)
    TAKE(PROP_RX_PREAMBLE, preamble)
    TAKE(PROP_RX_CRC, crc)
    TAKE(PROP_RX_IQ_INVERTED, iq_inverted)
    TAKE(PROP_RX_SYNC_WORD, sync_word)
#undef TAKE
    if (air.cr == 0u || air.cr > 4u) {
        air.cr = 1;
    }
    air.ldro = symbol_us(&air) >= 16000u;
    return air;
}

static bool injection_given(const struct sx126x_state *state, size_t property) {
    return (state->injection.set & (1u << (property - PROP_RX_FREQUENCY))) != 0u;
}

static int sx126x_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct sx126x_state *state = inst->state;
    struct sx126x_air radio = radio_air(state);
    struct sx126x_air next = injection_air(state);
    int64_t *v = &out->v[0];

    if (property >= PROP_TX_LENGTH && property <= PROP_TX_AIRTIME_US) {
        return tx_get(state, property, v);
    }
    switch (property) {
        case PROP_MODE: *v = state->mode; break;
        case PROP_PACKET_TYPE: *v = state->packet_type; break;
        case PROP_FREQUENCY: *v = radio.frequency_hz; break;
        case PROP_SF: *v = radio.sf; break;
        case PROP_BW: *v = radio.bw_hz; break;
        case PROP_CR: *v = radio.cr; break;
        case PROP_LDRO: *v = radio.ldro; break;
        case PROP_PREAMBLE: *v = radio.preamble; break;
        case PROP_IMPLICIT_HEADER: *v = radio.implicit_header; break;
        case PROP_PAYLOAD_LENGTH: *v = state->packet[3]; break;
        case PROP_CRC: *v = radio.crc; break;
        case PROP_IQ_INVERTED: *v = radio.iq_inverted; break;
        case PROP_SYNC_WORD: *v = radio.sync_word; break;
        case PROP_POWER: *v = output_power(state); break;
        case PROP_IRQ: *v = state->irq; break;
        case PROP_DIO2_RF_SWITCH: *v = state->dio2_rf_switch; break;
        case PROP_TCXO_MV: *v = state->tcxo_mv; break;
        case PROP_COMMANDS: *v = state->commands; break;
        case PROP_COMMAND_ERRORS: *v = state->command_errors; break;
        case PROP_RESETS: *v = state->resets; break;
        case PROP_SPI_MODE: return mclw_spi_mode_get(inst, BINDING_SPI, out);
        case PROP_NOISE_FLOOR: *v = state->noise_floor; break;
        case PROP_TX_PENDING: *v = state->tx_count; break;
        case PROP_TX_TOTAL: *v = state->tx_total; break;
        case PROP_TX_DROPPED: *v = state->tx_dropped; break;
        case PROP_RX_RECEIVED: *v = state->rx_received; break;
        case PROP_RX_MISSED: *v = state->rx_missed; break;
        case PROP_RX_FREQUENCY: *v = next.frequency_hz; break;
        case PROP_RX_SF: *v = next.sf; break;
        case PROP_RX_BW: *v = next.bw_hz; break;
        case PROP_RX_CR: *v = next.cr; break;
        case PROP_RX_PREAMBLE: *v = next.preamble; break;
        case PROP_RX_CRC: *v = next.crc; break;
        case PROP_RX_IQ_INVERTED: *v = next.iq_inverted; break;
        case PROP_RX_SYNC_WORD: *v = next.sync_word; break;
        case PROP_RX_RSSI: *v = injection_given(state, PROP_RX_RSSI) ? state->injection.rssi : RX_RSSI_DEFAULT; break;
        case PROP_RX_SNR: *v = injection_given(state, PROP_RX_SNR) ? state->injection.snr : RX_SNR_DEFAULT; break;
        case PROP_RX_SIGNAL_RSSI:
            *v = injection_given(state, PROP_RX_SIGNAL_RSSI) ? state->injection.signal_rssi :
                (injection_given(state, PROP_RX_RSSI) ? state->injection.rssi : RX_RSSI_DEFAULT);
            break;
        default: return -EINVAL;
    }
    return 0;
}

static int sx126x_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct sx126x_state *state = inst->state;
    struct sx126x_injection *next = &state->injection;
    int64_t v = in->v[0];

    switch (property) {
        case PROP_SPI_MODE:
            return mclw_spi_mode_set(inst, BINDING_SPI, in);
        case PROP_NOISE_FLOOR:
            state->noise_floor = (int16_t)v;
            publish(state);
            return 0;
        case PROP_TX_NEXT:
            if (v != 0 && state->tx_count > 0u) {
                state->tx_head = (uint8_t)((state->tx_head + 1u) % TX_QUEUE);
                state->tx_count--;
            }
            return 0;
        case PROP_RX_FREQUENCY: next->air.frequency_hz = (uint32_t)v; break;
        case PROP_RX_SF: next->air.sf = (uint8_t)v; break;
        case PROP_RX_BW: next->air.bw_hz = (uint32_t)v; break;
        case PROP_RX_CR: next->air.cr = (uint8_t)v; break;
        case PROP_RX_PREAMBLE: next->air.preamble = (uint16_t)v; break;
        case PROP_RX_CRC: next->air.crc = v != 0; break;
        case PROP_RX_IQ_INVERTED: next->air.iq_inverted = v != 0; break;
        case PROP_RX_SYNC_WORD: next->air.sync_word = (uint16_t)v; break;
        case PROP_RX_RSSI: next->rssi = (int16_t)v; break;
        case PROP_RX_SNR: next->snr = (int8_t)v; break;
        case PROP_RX_SIGNAL_RSSI: next->signal_rssi = (int16_t)v; break;
        default:
            return -EINVAL;
    }
    next->set |= 1u << (property - PROP_RX_FREQUENCY);
    return 0;
}

// A frame starts on air now. The radio receives it if it is listening on
// the frame's channel; the events then follow the air time.
static void inject(struct sx126x_state *state, const uint8_t *payload, size_t len) {
    struct sx126x_rx_frame *f = &state->rx;
    uint64_t now = mclw_now_us();
    struct sx126x_air air = injection_air(state);
    struct mclw_value value;
    uint32_t symbol = symbol_us(&air);
    uint32_t quarters = (air.sf < 7u ? 25u : 17u) + 4u * (uint32_t)air.preamble;

    if (f->stage != RX_IDLE) {
        state->rx_missed++;
    }
    memset(f, 0, sizeof(*f));
    f->air = air;
    f->length = (uint8_t)len;
    memcpy(f->payload, payload, len);
    sx126x_get(mclw_instance_from_state(state), PROP_RX_RSSI, &value);
    f->rssi = (int16_t)value.v[0];
    sx126x_get(mclw_instance_from_state(state), PROP_RX_SNR, &value);
    f->snr = (int8_t)value.v[0];
    sx126x_get(mclw_instance_from_state(state), PROP_RX_SIGNAL_RSSI, &value);
    f->signal_rssi = (int16_t)value.v[0];
    f->start_us = now;
    f->preamble_us = now + (uint64_t)symbol *
        (air.preamble < PREAMBLE_DETECT_SYMBOLS ? air.preamble : PREAMBLE_DETECT_SYMBOLS);
    f->header_us = now + ((uint64_t)symbol * (quarters + 32u)) / 4u;
    f->end_us = now + lora_airtime_us(&air, f->length);
    f->on_air = true;
    for (uint8_t i = 0; i < FAULT_COUNT; i++) {
        if (state->faults[i] > 0u) {
            state->faults[i]--;
            f->fault = (uint8_t)(i + 1u);
            break;
        }
    }
    memset(&state->injection, 0, sizeof(state->injection));
    if (receivable(state, &air)) {
        f->stage = RX_PREAMBLE;
    } else {
        state->rx_missed++;
    }
    (void)advance(state, now);
    publish(state);
}

static int sx126x_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf,
    size_t len) {
    const struct sx126x_tx_frame *f = front(inst->state);

    if (region != REGION_TX) {
        return -EINVAL;
    }
    if (f == NULL) {
        return -ENOENT;
    }
    memset(buf, 0, len);
    if (offset < f->length) {
        memcpy(buf, f->payload + offset, len < f->length - offset ? len : f->length - offset);
    }
    return 0;
}

static int sx126x_write_region(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf,
    size_t len) {
    if (region != REGION_RX || offset != 0u || len == 0u) {
        return -EINVAL;
    }
    inject(inst->state, buf, len);
    return 0;
}

static int sx126x_inject_fault(struct mclw_instance *inst, size_t fault, uint32_t count) {
    struct sx126x_state *state = inst->state;

    state->faults[fault] += count;
    return 0;
}

static int sx126x_clear_faults(struct mclw_instance *inst) {
    struct sx126x_state *state = inst->state;

    memset(state->faults, 0, sizeof(state->faults));
    return 0;
}

static const struct mclw_model_ops sx126x_ops = {
    .init = sx126x_init,
    .deinit = sx126x_deinit,
    .reset = sx126x_reset,
    .get = sx126x_get,
    .set = sx126x_set,
    .inject_fault = sx126x_inject_fault,
    .clear_faults = sx126x_clear_faults,
    .read_region = sx126x_read_region,
    .write_region = sx126x_write_region,
    .time_advanced = sx126x_time_advanced,
    .usb_rx = NULL,
};

// The SX1262 reports itself as an SX1261: RadioLib matches "SX1261" for both.
#define SX126X_MODEL(symbol, name, ver, ocp) \
    static const struct sx126x_config symbol##_config = { .version = ver, .ocp_reset = ocp }; \
    const struct mclw_model symbol = { \
        .abi_version = MCLW_ABI_VERSION, \
        .id = name, \
        .family = "sx126x", \
        .config = &symbol##_config, \
        .properties = sx126x_properties, \
        .property_count = sizeof(sx126x_properties) / sizeof(sx126x_properties[0]), \
        .regions = sx126x_regions, \
        .region_count = sizeof(sx126x_regions) / sizeof(sx126x_regions[0]), \
        .faults = sx126x_faults, \
        .fault_count = sizeof(sx126x_faults) / sizeof(sx126x_faults[0]), \
        .bindings = sx126x_bindings, \
        .binding_count = sizeof(sx126x_bindings) / sizeof(sx126x_bindings[0]), \
        .state_size = sizeof(struct sx126x_state), \
        .ops = &sx126x_ops, \
    }

SX126X_MODEL(mclw_model_sx1262, "sx1262", "SX1261 V2D 2D02", SX126X_OCP_SX1262);
SX126X_MODEL(mclw_model_sx1261, "sx1261", "SX1261 V2D 2D02", SX126X_OCP_SX1261);
SX126X_MODEL(mclw_model_sx1268, "sx1268", "SX1268 V2D 2D02", SX126X_OCP_SX1262);
SX126X_MODEL(mclw_model_llcc68, "llcc68", "LLCC68 V2D 2D02", SX126X_OCP_SX1262);
