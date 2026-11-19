// SPDX-License-Identifier: Apache-2.0 OR MIT
// i2c_page_controller: the harness as the I2C controller of a DUT that is
// the target, the I2C counterpart of spi_page_controller. Each page writes
// `out` to the DUT's address in `chunk`-byte write transfers, then reads
// 256 bytes back into `in` in `chunk`-byte read transfers, `pages` times,
// `interval` ms apart. With `offset`, every write starts with the page
// offset of its first byte, and every read is a write of that offset, a
// repeated START and the read: how an EEPROM-style target (pico-examples'
// slave_mem_i2c) takes an address. A transfer the DUT ends with a NACK
// counts in `nacks`, its cause in `last_error`, and the page goes on with
// its next chunk.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/i2c_controller.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "i2c_page.h"

enum {
    BINDING_SDA,
    BINDING_SCL,
};

enum {
    REGION_OUT,
    REGION_IN,
};

enum {
    PROP_ADDRESS,
    PROP_PAGES,
    PROP_INTERVAL,
    PROP_RATE,
    PROP_CHUNK,
    PROP_OFFSET,
    PROP_CLOCKED,
    PROP_TRANSFERS,
    PROP_NACKS,
    PROP_LAST_ERROR,
};

struct i2c_page_state {
    struct mclw_instance *inst;
    struct mclw_i2c_controller *ctl;
    uint8_t out[I2C_PAGE_SIZE];
    uint8_t in[I2C_PAGE_SIZE];
    uint8_t incoming[I2C_PAGE_SIZE];
    // A write's bytes, the offset first when `offset` is on.
    uint8_t buffer[1u + I2C_PAGE_SIZE];
    uint32_t address;
    uint32_t pages;
    uint32_t interval_ms;
    uint32_t rate_khz;
    uint32_t chunk;
    bool offset;
    uint32_t clocked;
    uint32_t transfers;
    uint32_t nacks;
    uint32_t last_error;
    // The page in flight: writing or reading, where the next chunk starts,
    // and whether a transfer is out.
    bool in_flight;
    bool reading;
    uint32_t at;
    bool transfer_out;
    uint64_t next_us;
};

#define COUNTER(prop_name) \
    { .name = prop_name, .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, \
      .unit = NULL, .min = 0, .max = UINT32_MAX }

static const struct mclw_property_desc properties[] = {
    { .name = "address", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0x08, .max = 0x77 },
    { .name = "pages", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = 100000 },
    { .name = "interval", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "ms",
      .min = 1, .max = 60000 },
    { .name = "rate", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "kHz",
      .min = 100, .max = 400 },
    { .name = "chunk", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, .unit = "bytes",
      .min = 1, .max = I2C_PAGE_SIZE },
    { .name = "offset", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RW, .unit = NULL,
      .min = 0, .max = 1 },
    COUNTER("clocked"),
    COUNTER("transfers"),
    COUNTER("nacks"),
    COUNTER("last_error"),
};

static const struct mclw_region_desc regions[] = {
    { .name = "out", .size = I2C_PAGE_SIZE, .access = MCLW_ACCESS_RW },
    { .name = "in", .size = I2C_PAGE_SIZE, .access = MCLW_ACCESS_RO },
};

#define PIN(binding_name) \
    { .name = binding_name, .kind = MCLW_BINDING_GPIO_OUT, .required = true, .usb = NULL }

static const struct mclw_binding_desc bindings[] = {
    PIN("sda"),
    PIN("scl"),
};

// What spi_page_controller sends: out[i] = i.
static void default_page(struct i2c_page_state *s) {
    for (size_t i = 0; i < I2C_PAGE_SIZE; i++) {
        s->out[i] = (uint8_t)i;
    }
}

static int page_init(struct mclw_instance *inst, const struct mclw_binding *b) {
    struct i2c_page_state *s = inst->state;
    int err;

    s->inst = inst;
    s->interval_ms = I2C_PAGE_INTERVAL_MS;
    s->rate_khz = I2C_PAGE_RATE_KHZ;
    s->chunk = I2C_PAGE_CHUNK;
    default_page(s);
    s->ctl = mclw_hal_i2c_controller_open(b[BINDING_SDA].gpio, b[BINDING_SCL].gpio, s->rate_khz * 1000u, &err);
    return s->ctl == NULL ? err : 0;
}

static void page_deinit(struct mclw_instance *inst) {
    struct i2c_page_state *s = inst->state;

    mclw_hal_i2c_controller_close(s->ctl);
    s->ctl = NULL;
}

static int page_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct i2c_page_state *s = inst->state;
    const uint32_t values[] = {
        [PROP_ADDRESS] = s->address,       [PROP_PAGES] = s->pages,     [PROP_INTERVAL] = s->interval_ms,
        [PROP_RATE] = s->rate_khz,         [PROP_CHUNK] = s->chunk,     [PROP_OFFSET] = s->offset ? 1u : 0u,
        [PROP_CLOCKED] = s->clocked,       [PROP_TRANSFERS] = s->transfers, [PROP_NACKS] = s->nacks,
        [PROP_LAST_ERROR] = s->last_error,
    };

    if (property >= sizeof(values) / sizeof(values[0])) {
        return -EINVAL;
    }
    out->v[0] = values[property];
    return 0;
}

// The first page waits one interval too: a DUT just started gets that long
// to be listening. A page needs the DUT's address.
static int page_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct i2c_page_state *s = inst->state;
    uint32_t v = (uint32_t)in->v[0];

    switch (property) {
        case PROP_ADDRESS:
            if (s->in_flight) {
                return -EBUSY;
            }
            s->address = v;
            return 0;
        case PROP_PAGES:
            if (v != 0u && s->address == 0u) {
                return -EINVAL;
            }
            s->pages = v;
            s->next_us = mclw_now_us() + (uint64_t)s->interval_ms * 1000u;
            return 0;
        case PROP_INTERVAL:
            s->interval_ms = v;
            return 0;
        case PROP_RATE:
            if (v != 100u && v != 400u) {
                return -EINVAL;
            }
            if (mclw_hal_i2c_controller_set_rate(s->ctl, v * 1000u) != 0) {
                return -EBUSY;
            }
            s->rate_khz = v;
            return 0;
        case PROP_CHUNK:
            s->chunk = v;
            return 0;
        case PROP_OFFSET:
            s->offset = v != 0u;
            return 0;
        default:
            return -EINVAL;
    }
}

static int page_reset(struct mclw_instance *inst, bool factory) {
    struct i2c_page_state *s = inst->state;

    if (s->transfer_out) {
        return -EBUSY;
    }
    if (factory) {
        default_page(s);
    }
    s->pages = 0u;
    s->in_flight = false;
    s->clocked = 0u;
    s->transfers = 0u;
    s->nacks = 0u;
    s->last_error = 0u;
    return 0;
}

static int page_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    const struct i2c_page_state *s = inst->state;

    if (region == REGION_OUT) {
        memcpy(buf, &s->out[offset], len);
        return 0;
    }
    if (region == REGION_IN) {
        memcpy(buf, &s->in[offset], len);
        return 0;
    }
    return -EINVAL;
}

static int page_write_region(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf, size_t len) {
    struct i2c_page_state *s = inst->state;

    if (region != REGION_OUT) {
        return -EINVAL;
    }
    if (s->in_flight) {
        return -EBUSY;
    }
    memcpy(&s->out[offset], buf, len);
    return 0;
}

// The next chunk's transfer: a write of out[at..], or a read into
// incoming[at..], each with the offset byte first when `offset` is on.
static void start_chunk(struct i2c_page_state *s) {
    uint32_t n = I2C_PAGE_SIZE - s->at < s->chunk ? I2C_PAGE_SIZE - s->at : s->chunk;
    size_t head = s->offset ? 1u : 0u;

    s->buffer[0] = (uint8_t)s->at;
    if (!s->reading) {
        memcpy(&s->buffer[head], &s->out[s->at], n);
    }
    int err = s->reading
        ? mclw_hal_i2c_controller_transfer(s->ctl, (uint8_t)s->address, s->buffer, head, &s->incoming[s->at], n)
        : mclw_hal_i2c_controller_transfer(s->ctl, (uint8_t)s->address, s->buffer, head + n, NULL, 0u);

    if (err == 0) {
        s->transfers++;
        s->transfer_out = true;
    }
}

// A chunk's end: a NACK or a held bus counts and the page goes on.
static void end_chunk(struct i2c_page_state *s) {
    enum mclw_i2c_result result = mclw_hal_i2c_controller_result(s->ctl, NULL);

    s->transfer_out = false;
    if (result != MCLW_I2C_OK) {
        s->last_error = (uint32_t)result;
        if (result == MCLW_I2C_ADDRESS_NACK || result == MCLW_I2C_DATA_NACK) {
            s->nacks++;
        }
    }
    mclw_trace(s->inst, MCLW_TRACE_I2C, s->at, (uint16_t)s->chunk, -(int)result);
    s->at += s->chunk;
    if (s->at < I2C_PAGE_SIZE) {
        return;
    }
    s->at = 0u;
    if (!s->reading) {
        s->reading = true;
        return;
    }
    memcpy(s->in, s->incoming, sizeof(s->in));
    s->in_flight = false;
    s->reading = false;
    s->clocked++;
    s->pages--;
    s->next_us = mclw_now_us() + (uint64_t)s->interval_ms * 1000u;
}

static void page_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct i2c_page_state *s = inst->state;

    if (s->transfer_out) {
        if (mclw_hal_i2c_controller_busy(s->ctl)) {
            return;
        }
        end_chunk(s);
    }
    if (!s->in_flight && s->pages != 0u && now_us >= s->next_us) {
        s->in_flight = true;
        s->reading = false;
        s->at = 0u;
        memset(s->incoming, 0, sizeof(s->incoming));
    }
    if (s->in_flight && !s->transfer_out) {
        start_chunk(s);
    }
}

static const struct mclw_model_ops ops = {
    .init = page_init,
    .deinit = page_deinit,
    .reset = page_reset,
    .get = page_get,
    .set = page_set,
    .read_region = page_read_region,
    .write_region = page_write_region,
    .time_advanced = page_time_advanced,
};

const struct mclw_model mclw_model_i2c_page_controller = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "i2c_page_controller",
    .family = "i2c_page",
    .config = NULL,
    .properties = properties,
    .property_count = sizeof(properties) / sizeof(properties[0]),
    .regions = regions,
    .region_count = sizeof(regions) / sizeof(regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = bindings,
    .binding_count = sizeof(bindings) / sizeof(bindings[0]),
    .state_size = sizeof(struct i2c_page_state),
    .ops = &ops,
};
