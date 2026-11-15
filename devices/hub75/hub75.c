// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/wire.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "hub75.h"

enum {
    PROP_LATCHES,
    PROP_ROWS_SEEN,
    PROP_SLOTS,
    PROP_UNMATCHED,
    PROP_SHORT,
};

enum {
    BINDING_UPPER,
    BINDING_LOWER,
    BINDING_ROWS,
    BINDING_E,
    BINDING_CLOCK,
};

enum {
    REGION_PLANES,
    REGION_THUMBNAIL,
};

// Data pins in a column's byte: R0, G0, B0, R1, G1, B1 in bits 0..5.
#define DATA_PINS 6u
#define ADDRESS_PINS 5u

struct hub75_slot {
    uint32_t enabled_ns;
    uint8_t columns[HUB75_THUMB_COLUMNS];
};

struct hub75_state {
    struct mclw_wire *clock;
    int8_t data_bit[DATA_PINS];
    int8_t address_bit[ADDRESS_PINS];
    int8_t clock_bit;
    uint32_t latches;
    uint32_t rows_seen;
    uint32_t unmatched;
    uint32_t short_latches;
    struct hub75_slot slots[HUB75_SCAN_ROWS][HUB75_SLOTS];
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

static const struct mclw_property_desc hub75_properties[] = {
    COUNTER("latches"),
    COUNTER("rows_seen"),
    COUNTER("slots"),
    COUNTER("unmatched"),
    COUNTER("short"),
};

static const struct mclw_region_desc hub75_regions[] = {
    { .name = "planes", .size = HUB75_PLANES_BYTES, .access = MCLW_ACCESS_RO },
    { .name = "thumbnail", .size = HUB75_THUMBNAIL_BYTES, .access = MCLW_ACCESS_RO },
};

#define PINS_BINDING(binding_name, count) \
    { \
        .name = (binding_name), \
        .kind = MCLW_BINDING_WIRE, \
        .required = true, \
        .usb = NULL, \
        .spi = { 0 }, \
        .uart = { 0 }, \
        .wire = { .engine = MCLW_WIRE_PINS, .pins = (count) }, \
    }

// The clock comes last: its engine takes a PIO block whose window reaches
// the pins the bindings before it opened.
static const struct mclw_binding_desc hub75_bindings[] = {
    PINS_BINDING("upper", 3),
    PINS_BINDING("lower", 3),
    PINS_BINDING("rows", 4),
    PINS_BINDING("e", 1),
    {
        .name = "clock",
        .kind = MCLW_BINDING_WIRE,
        .required = true,
        .usb = NULL,
        .spi = { 0 },
        .uart = { 0 },
        .wire = { .engine = MCLW_WIRE_LATCHED_IN, .pins = 3 },
    },
};

static bool level(uint32_t sample, int8_t bit) {
    return ((sample >> bit) & 1u) != 0u;
}

// One latch: the last 128 bits clocked in before it are the shift
// registers' contents, the first of them now at the far end of the chain
// (column 0), the address lines say which row they light, and the time the
// enable line was asserted until the next latch says for how long.
// Latches of one row are told apart by that time.
static void hub75_take(struct hub75_state *state, const uint32_t *words, size_t n) {
    if (n < HUB75_COLUMNS + 2u) {
        state->short_latches++;
        return;
    }
    uint32_t enabled_ns = words[n - 1u];
    uint32_t latch = words[n - 2u];
    const uint32_t *columns = &words[n - 2u - HUB75_COLUMNS];
    uint32_t row = 0;

    for (uint32_t i = 0; i < n - 2u; i++) {
        if (!level(words[i], state->clock_bit)) {
            state->short_latches++;
            return;
        }
    }
    for (uint32_t a = 0; a < ADDRESS_PINS; a++) {
        row |= (level(latch, state->address_bit[a]) ? 1u : 0u) << a;
    }
    struct hub75_slot *slots = state->slots[row];
    struct hub75_slot *slot = NULL;
    for (uint32_t s = 0; s < HUB75_SLOTS && slot == NULL; s++) {
        uint32_t t = slots[s].enabled_ns;
        if (t != 0u && enabled_ns + t / 8u >= t && enabled_ns <= t + t / 8u) {
            slot = &slots[s];
        }
    }
    for (uint32_t s = 0; s < HUB75_SLOTS && slot == NULL; s++) {
        if (slots[s].enabled_ns == 0u) {
            slot = &slots[s];
        }
    }
    if (slot == NULL || enabled_ns == 0u) {
        state->unmatched++;
        return;
    }
    slot->enabled_ns = enabled_ns;
    for (uint32_t c = 0; c < HUB75_THUMB_COLUMNS; c++) {
        uint32_t sample = columns[c * HUB75_THUMB_STEP];
        uint8_t bits = 0;
        for (uint32_t d = 0; d < DATA_PINS; d++) {
            bits |= (uint8_t)((level(sample, state->data_bit[d]) ? 1u : 0u) << d);
        }
        slot->columns[c] = bits;
    }
    state->rows_seen |= 1u << row;
    state->latches++;
}

static void hub75_poll(struct mclw_instance *inst) {
    struct hub75_state *state = inst->state;
    uint32_t words[HUB75_CAPTURE_WORDS];
    size_t n = mclw_hal_wire_read(state->clock, words, HUB75_CAPTURE_WORDS);

    if (n > 0u) {
        hub75_take(state, words, n);
        (void)mclw_hal_wire_capture(state->clock);
    }
}

// What a viewer sees of a thumbnail pixel: each colour lit for the share
// of its row's enabled time during which its bit was latched on, out of
// 255. `at` is the byte's index in the thumbnail region.
static uint8_t thumbnail_byte(const struct hub75_state *state, uint32_t at) {
    uint32_t y = at / (HUB75_THUMB_COLUMNS * 3u);
    uint32_t c = at / 3u % HUB75_THUMB_COLUMNS;
    uint32_t bit = y / HUB75_SCAN_ROWS * 3u + at % 3u;
    const struct hub75_slot *slots = state->slots[y % HUB75_SCAN_ROWS];
    uint64_t on = 0;
    uint64_t total = 0;

    for (uint32_t s = 0; s < HUB75_SLOTS; s++) {
        total += slots[s].enabled_ns;
        if (((slots[s].columns[c] >> bit) & 1u) != 0u) {
            on += slots[s].enabled_ns;
        }
    }
    return total == 0u ? 0u : (uint8_t)((on * 255u + total / 2u) / total);
}

static int find_bits(struct hub75_state *state, const struct mclw_binding *bindings) {
    const struct mclw_wire *clock = bindings[BINDING_CLOCK].wire;
    const struct mclw_wire *groups[] = {
        bindings[BINDING_UPPER].wire, bindings[BINDING_LOWER].wire, bindings[BINDING_ROWS].wire,
        bindings[BINDING_E].wire,
    };
    static const uint8_t counts[] = { 3u, 3u, 4u, 1u };
    int8_t bits[DATA_PINS + ADDRESS_PINS];
    size_t n = 0;

    for (size_t g = 0; g < sizeof(counts); g++) {
        for (uint8_t i = 0; i < counts[g]; i++) {
            int bit = mclw_hal_wire_sample_bit(clock, mclw_hal_wire_pin(groups[g], i));
            if (bit < 0) {
                return -EINVAL;
            }
            bits[n++] = (int8_t)bit;
        }
    }
    memcpy(state->data_bit, bits, DATA_PINS);
    memcpy(state->address_bit, &bits[DATA_PINS], ADDRESS_PINS);
    state->clock_bit = (int8_t)mclw_hal_wire_sample_bit(clock, mclw_hal_wire_pin(clock, 0));
    return state->clock_bit < 0 ? -EINVAL : 0;
}

static void forget(struct hub75_state *state) {
    state->latches = 0;
    state->rows_seen = 0;
    state->unmatched = 0;
    state->short_latches = 0;
    memset(state->slots, 0, sizeof(state->slots));
}

static int hub75_init(struct mclw_instance *inst, const struct mclw_binding *bindings) {
    struct hub75_state *state = inst->state;
    int err = find_bits(state, bindings);

    if (err != 0) {
        return err;
    }
    state->clock = bindings[BINDING_CLOCK].wire;
    forget(state);
    return mclw_hal_wire_capture(state->clock);
}

static void hub75_deinit(struct mclw_instance *inst) {
    struct hub75_state *state = inst->state;

    state->clock = NULL;
}

static int hub75_reset(struct mclw_instance *inst, bool factory) {
    (void)factory;
    forget(inst->state);
    return 0;
}

static int hub75_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct hub75_state *state = inst->state;
    uint32_t slots = 0;

    switch (property) {
        case PROP_LATCHES:
            out->v[0] = state->latches;
            return 0;
        case PROP_ROWS_SEEN:
            out->v[0] = state->rows_seen;
            return 0;
        case PROP_SLOTS:
            for (uint32_t r = 0; r < HUB75_SCAN_ROWS; r++) {
                for (uint32_t s = 0; s < HUB75_SLOTS; s++) {
                    slots += state->slots[r][s].enabled_ns != 0u ? 1u : 0u;
                }
            }
            out->v[0] = slots;
            return 0;
        case PROP_UNMATCHED:
            out->v[0] = state->unmatched;
            return 0;
        case PROP_SHORT:
            out->v[0] = state->short_latches;
            return 0;
        default:
            return -EINVAL;
    }
}

// planes: per scan row, per slot, the enabled time (4 bytes, little
// endian) and a byte per thumbnail column. thumbnail: per panel row and
// thumbnail column, red, green and blue.
static int hub75_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    const struct hub75_state *state = inst->state;
    uint8_t *dst = buf;

    if (region == REGION_THUMBNAIL) {
        if (offset > HUB75_THUMBNAIL_BYTES || len > HUB75_THUMBNAIL_BYTES - offset) {
            return -EINVAL;
        }
        for (size_t i = 0; i < len; i++) {
            dst[i] = thumbnail_byte(state, offset + (uint32_t)i);
        }
        return 0;
    }
    if (region != REGION_PLANES || offset > HUB75_PLANES_BYTES || len > HUB75_PLANES_BYTES - offset) {
        return -EINVAL;
    }
    for (size_t i = 0; i < len; i++) {
        uint32_t at = offset + (uint32_t)i;
        const struct hub75_slot *slot = &state->slots[at / HUB75_ROW_BYTES][at % HUB75_ROW_BYTES / HUB75_SLOT_BYTES];
        uint32_t within = at % HUB75_SLOT_BYTES;
        dst[i] = within < 4u ? (uint8_t)(slot->enabled_ns >> (8u * within)) : slot->columns[within - 4u];
    }
    return 0;
}

static const struct mclw_model_ops hub75_ops = {
    .init = hub75_init,
    .deinit = hub75_deinit,
    .reset = hub75_reset,
    .get = hub75_get,
    .set = NULL,
    .inject_fault = NULL,
    .clear_faults = NULL,
    .read_region = hub75_read_region,
    .write_region = NULL,
    .time_advanced = NULL,
    .poll = hub75_poll,
    .usb_rx = NULL,
    .usb_control = NULL,
};

const struct mclw_model mclw_model_hub75 = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "hub75",
    .family = "hub75",
    .config = NULL,
    .properties = hub75_properties,
    .property_count = sizeof(hub75_properties) / sizeof(hub75_properties[0]),
    .regions = hub75_regions,
    .region_count = sizeof(hub75_regions) / sizeof(hub75_regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = hub75_bindings,
    .binding_count = sizeof(hub75_bindings) / sizeof(hub75_bindings[0]),
    .state_size = sizeof(struct hub75_state),
    .ops = &hub75_ops,
};
