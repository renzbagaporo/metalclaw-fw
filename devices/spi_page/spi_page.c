// SPDX-License-Identifier: Apache-2.0 OR MIT
// The other end of an SPI link that moves 256-byte pages one byte per
// chip-select window, as two PL022s in mode 0 do: pico-examples'
// spi_master and spi_slave, each the other's part. Mode 0 unless the
// `mode` property says otherwise.
//
// spi_page_target       for a DUT that is the controller: every window
//                       answers the next byte of its `miso` page and keeps
//                       the byte it got at the same place in `mosi`.
// spi_page_controller   for a DUT that is the target: clocks its `mosi`
//                       page `pages` times, `interval` ms apart, and keeps
//                       what came back from the last one in `miso`.

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/spi_peer.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

#include "spi_page.h"

enum {
    BINDING_SCK,
    BINDING_MOSI,
    BINDING_MISO,
    BINDING_CS,
};

enum {
    REGION_MISO,
    REGION_MOSI,
};

enum {
    TARGET_PROP_BYTES,
    TARGET_PROP_MODE,
};

enum {
    CONTROLLER_PROP_PAGES,
    CONTROLLER_PROP_INTERVAL,
    CONTROLLER_PROP_CLOCKED,
    CONTROLLER_PROP_MODE,
};

struct spi_page_state {
    struct mclw_instance *inst;
    struct mclw_spi_peer *peer;
    uint8_t miso[SPI_PAGE_SIZE];
    uint8_t mosi[SPI_PAGE_SIZE];
    // Target: bytes before the last realignment, and how long the count
    // has stood still part way through a page.
    uint32_t earlier;
    uint32_t last_count;
    uint32_t still_polls;
    // Controller: what the page in flight brings back, and the schedule.
    uint8_t incoming[SPI_PAGE_SIZE];
    bool in_flight;
    uint32_t pages;
    uint32_t interval_ms;
    uint32_t clocked;
    uint64_t next_us;
    uint8_t mode;
};

#define COUNTER(prop_name, prop_unit) \
    { .name = prop_name, .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, \
      .unit = prop_unit, .min = 0, .max = UINT32_MAX }

// CPOL in bit 1, CPHA in bit 0.
#define MODE_PROPERTY \
    { .name = "mode", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, \
      .unit = NULL, .min = 0, .max = 3 }

static const struct mclw_property_desc target_properties[] = {
    COUNTER("bytes", "bytes"),
    MODE_PROPERTY,
};

static const struct mclw_property_desc controller_properties[] = {
    {
        .name = "pages",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 0,
        .max = 100000,
    },
    {
        .name = "interval",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "ms",
        .min = 1,
        .max = 60000,
    },
    COUNTER("clocked", NULL),
    MODE_PROPERTY,
};

// The target answers with `miso` and fills `mosi`; the controller sends
// `mosi` and fills `miso`.
static const struct mclw_region_desc target_regions[] = {
    { .name = "miso", .size = SPI_PAGE_SIZE, .access = MCLW_ACCESS_RW },
    { .name = "mosi", .size = SPI_PAGE_SIZE, .access = MCLW_ACCESS_RO },
};

static const struct mclw_region_desc controller_regions[] = {
    { .name = "miso", .size = SPI_PAGE_SIZE, .access = MCLW_ACCESS_RO },
    { .name = "mosi", .size = SPI_PAGE_SIZE, .access = MCLW_ACCESS_RW },
};

#define PIN(binding_name, binding_kind) \
    { .name = binding_name, .kind = binding_kind, .required = true, .usb = NULL }

static const struct mclw_binding_desc target_bindings[] = {
    PIN("sck", MCLW_BINDING_GPIO_IN),
    PIN("mosi", MCLW_BINDING_GPIO_IN),
    PIN("miso", MCLW_BINDING_GPIO_OUT),
    PIN("cs", MCLW_BINDING_GPIO_IN),
};

static const struct mclw_binding_desc controller_bindings[] = {
    PIN("sck", MCLW_BINDING_GPIO_OUT),
    PIN("mosi", MCLW_BINDING_GPIO_OUT),
    PIN("miso", MCLW_BINDING_GPIO_IN),
    PIN("cs", MCLW_BINDING_GPIO_OUT),
};

// What each one's partner program sends: spi_slave.c out_buf[i] = ~i,
// spi_master.c out_buf[i] = i.
static void default_pages(struct spi_page_state *s, bool controller) {
    for (size_t i = 0; i < SPI_PAGE_SIZE; i++) {
        uint8_t up = (uint8_t)i;

        s->miso[i] = controller ? 0u : (uint8_t)~up;
        s->mosi[i] = controller ? up : 0u;
    }
}

static int target_init(struct mclw_instance *inst, const struct mclw_binding *b) {
    struct spi_page_state *s = inst->state;
    int err;

    s->inst = inst;
    default_pages(s, false);
    s->peer = mclw_hal_spi_peer_target(b[BINDING_SCK].gpio, b[BINDING_MOSI].gpio, b[BINDING_MISO].gpio,
        b[BINDING_CS].gpio, &err);
    if (s->peer == NULL) {
        return err;
    }
    mclw_hal_spi_peer_load(s->peer, s->miso, sizeof(s->miso));
    return 0;
}

static int controller_init(struct mclw_instance *inst, const struct mclw_binding *b) {
    struct spi_page_state *s = inst->state;
    int err;

    s->inst = inst;
    s->interval_ms = SPI_PAGE_INTERVAL_MS;
    default_pages(s, true);
    s->peer = mclw_hal_spi_peer_controller(b[BINDING_SCK].gpio, b[BINDING_MOSI].gpio, b[BINDING_MISO].gpio,
        b[BINDING_CS].gpio, SPI_PAGE_HZ, &err);
    return s->peer == NULL ? err : 0;
}

static void page_deinit(struct mclw_instance *inst) {
    struct spi_page_state *s = inst->state;

    mclw_hal_spi_peer_close(s->peer);
    s->peer = NULL;
}

static uint32_t target_bytes(const struct spi_page_state *s) {
    return s->earlier + mclw_hal_spi_peer_count(s->peer);
}

static int target_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct spi_page_state *s = inst->state;

    switch (property) {
        case TARGET_PROP_BYTES:
            out->v[0] = target_bytes(s);
            return 0;
        case TARGET_PROP_MODE:
            out->v[0] = s->mode;
            return 0;
        default:
            return -EINVAL;
    }
}

// The rings start again at byte 0 in the new mode, as after a realignment.
static int target_set_mode(struct spi_page_state *s, uint8_t mode) {
    uint32_t before = target_bytes(s);
    int err = mclw_hal_spi_peer_set_mode(s->peer, mode);

    if (err != 0) {
        return err;
    }
    s->mode = mode;
    s->earlier = before;
    s->last_count = 0u;
    s->still_polls = 0u;
    return 0;
}

static int target_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct spi_page_state *s = inst->state;

    if (property != TARGET_PROP_MODE) {
        return -EINVAL;
    }
    return target_set_mode(s, (uint8_t)in->v[0]);
}

static int target_reset(struct mclw_instance *inst, bool factory) {
    struct spi_page_state *s = inst->state;

    if (factory) {
        default_pages(s, false);
        if (target_set_mode(s, 0u) != 0) {
            return -EIO;
        }
    }
    s->earlier = 0u;
    s->still_polls = 0u;
    mclw_hal_spi_peer_load(s->peer, s->miso, sizeof(s->miso));
    s->last_count = 0u;
    return 0;
}

static int target_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    struct spi_page_state *s = inst->state;

    if (region == REGION_MISO) {
        memcpy(buf, &s->miso[offset], len);
        return 0;
    }
    if (region == REGION_MOSI) {
        mclw_hal_spi_peer_received(s->peer, offset, buf, len);
        return 0;
    }
    return -EINVAL;
}

// A new page is served from the next window, at ring position 0.
static int target_write_region(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf,
    size_t len) {
    struct spi_page_state *s = inst->state;

    if (region != REGION_MISO) {
        return -EINVAL;
    }
    memcpy(&s->miso[offset], buf, len);
    s->earlier = target_bytes(s);
    mclw_hal_spi_peer_load(s->peer, s->miso, sizeof(s->miso));
    s->last_count = 0u;
    return 0;
}

// Every page the DUT clocks is a whole number of windows, so a count that
// stops part way through one is a page the DUT abandoned; from there the
// rings would be off by that much for every page after.
static void target_poll(struct mclw_instance *inst) {
    struct spi_page_state *s = inst->state;
    uint32_t count = mclw_hal_spi_peer_count(s->peer);

    if (count % SPI_PAGE_SIZE == 0u || count != s->last_count) {
        s->last_count = count;
        s->still_polls = 0u;
        return;
    }
    if (++s->still_polls >= SPI_PAGE_REALIGN_POLLS) {
        s->earlier += count;
        mclw_hal_spi_peer_restart(s->peer);
        mclw_trace(inst, MCLW_TRACE_STATE, count, 0u, 0);
        s->last_count = 0u;
        s->still_polls = 0u;
    }
}

static int controller_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct spi_page_state *s = inst->state;

    switch (property) {
        case CONTROLLER_PROP_PAGES:
            out->v[0] = s->pages;
            return 0;
        case CONTROLLER_PROP_INTERVAL:
            out->v[0] = s->interval_ms;
            return 0;
        case CONTROLLER_PROP_CLOCKED:
            out->v[0] = s->clocked;
            return 0;
        case CONTROLLER_PROP_MODE:
            out->v[0] = s->mode;
            return 0;
        default:
            return -EINVAL;
    }
}

// The first page waits one interval too: a DUT just started gets that long
// to be listening.
static int controller_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct spi_page_state *s = inst->state;

    switch (property) {
        case CONTROLLER_PROP_PAGES:
            s->pages = (uint32_t)in->v[0];
            s->next_us = mclw_now_us() + (uint64_t)s->interval_ms * 1000u;
            return 0;
        case CONTROLLER_PROP_INTERVAL:
            s->interval_ms = (uint32_t)in->v[0];
            return 0;
        case CONTROLLER_PROP_MODE: {
            // Between pages: -EBUSY while one is in flight.
            int err = mclw_hal_spi_peer_set_mode(s->peer, (uint8_t)in->v[0]);

            if (err == 0) {
                s->mode = (uint8_t)in->v[0];
            }
            return err;
        }
        default:
            return -EINVAL;
    }
}

static int controller_reset(struct mclw_instance *inst, bool factory) {
    struct spi_page_state *s = inst->state;

    if (factory) {
        default_pages(s, true);
        s->interval_ms = SPI_PAGE_INTERVAL_MS;
        if (mclw_hal_spi_peer_set_mode(s->peer, 0u) == 0) {
            s->mode = 0u;
        }
    }
    s->pages = 0u;
    s->clocked = 0u;
    return 0;
}

static int controller_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf,
    size_t len) {
    const struct spi_page_state *s = inst->state;

    if (region == REGION_MISO) {
        memcpy(buf, &s->miso[offset], len);
        return 0;
    }
    if (region == REGION_MOSI) {
        memcpy(buf, &s->mosi[offset], len);
        return 0;
    }
    return -EINVAL;
}

static int controller_write_region(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf,
    size_t len) {
    struct spi_page_state *s = inst->state;

    if (region != REGION_MOSI || s->in_flight) {
        return region != REGION_MOSI ? -EINVAL : -EBUSY;
    }
    memcpy(&s->mosi[offset], buf, len);
    return 0;
}

static void controller_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct spi_page_state *s = inst->state;

    if (s->in_flight) {
        if (mclw_hal_spi_peer_busy(s->peer)) {
            return;
        }
        memcpy(s->miso, s->incoming, sizeof(s->miso));
        s->in_flight = false;
        s->clocked++;
        s->pages--;
        s->next_us = now_us + (uint64_t)s->interval_ms * 1000u;
        mclw_trace(inst, MCLW_TRACE_SPI, s->clocked, SPI_PAGE_SIZE, 0);
    }
    if (s->pages != 0u && now_us >= s->next_us &&
        mclw_hal_spi_peer_transfer(s->peer, s->mosi, s->incoming, SPI_PAGE_SIZE) == 0) {
        s->in_flight = true;
    }
}

static const struct mclw_model_ops target_ops = {
    .init = target_init,
    .deinit = page_deinit,
    .reset = target_reset,
    .get = target_get,
    .set = target_set,
    .read_region = target_read_region,
    .write_region = target_write_region,
    .poll = target_poll,
};

static const struct mclw_model_ops controller_ops = {
    .init = controller_init,
    .deinit = page_deinit,
    .reset = controller_reset,
    .get = controller_get,
    .set = controller_set,
    .read_region = controller_read_region,
    .write_region = controller_write_region,
    .time_advanced = controller_time_advanced,
};

const struct mclw_model mclw_model_spi_page_target = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "spi_page_target",
    .family = "spi_page",
    .config = NULL,
    .properties = target_properties,
    .property_count = sizeof(target_properties) / sizeof(target_properties[0]),
    .regions = target_regions,
    .region_count = sizeof(target_regions) / sizeof(target_regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = target_bindings,
    .binding_count = sizeof(target_bindings) / sizeof(target_bindings[0]),
    .state_size = sizeof(struct spi_page_state),
    .ops = &target_ops,
};

const struct mclw_model mclw_model_spi_page_controller = {
    .abi_version = MCLW_ABI_VERSION,
    .id = "spi_page_controller",
    .family = "spi_page",
    .config = NULL,
    .properties = controller_properties,
    .property_count = sizeof(controller_properties) / sizeof(controller_properties[0]),
    .regions = controller_regions,
    .region_count = sizeof(controller_regions) / sizeof(controller_regions[0]),
    .faults = NULL,
    .fault_count = 0,
    .bindings = controller_bindings,
    .binding_count = sizeof(controller_bindings) / sizeof(controller_bindings[0]),
    .state_size = sizeof(struct spi_page_state),
    .ops = &controller_ops,
};
