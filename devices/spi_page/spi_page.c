// SPDX-License-Identifier: Apache-2.0 OR MIT
// The other end of an SPI link that moves 256-byte pages a frame at a
// time: by default one byte per chip-select window, mode 0, as two PL022s
// in mode 0 do (pico-examples' spi_master and spi_slave, each the other's
// part), and in any SPI mode, 8- or 16-bit frames, MSB or LSB first, and
// windows of any length otherwise.
//
// spi_page_target       for a DUT that is the controller: every frame
//                       answers the next frame of its `miso` page and keeps
//                       the frame it got at the same place in `mosi`.
// spi_page_controller   for a DUT that is the target: clocks `length` bytes
//                       of its `mosi` page `pages` times, `interval` ms
//                       apart, in windows of `window` frames, and keeps
//                       what comes back in `miso` as it comes.

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
    TARGET_PROP_BITS,
    TARGET_PROP_LSB_FIRST,
};

enum {
    CONTROLLER_PROP_PAGES,
    CONTROLLER_PROP_INTERVAL,
    CONTROLLER_PROP_CLOCKED,
    CONTROLLER_PROP_MODE,
    CONTROLLER_PROP_BITS,
    CONTROLLER_PROP_LSB_FIRST,
    CONTROLLER_PROP_LENGTH,
    CONTROLLER_PROP_WINDOW,
};

struct spi_page_state {
    struct mclw_instance *inst;
    struct mclw_spi_peer *peer;
    struct mclw_spi_peer_format format;
    uint8_t miso[SPI_PAGE_SIZE];
    uint8_t mosi[SPI_PAGE_SIZE];
    // Target: bytes before the last realignment, and since when the count
    // has stood still part way through a page.
    uint32_t earlier;
    uint32_t last_count;
    uint64_t still_since_us;
    // Controller: what the page in flight brings back, and the schedule.
    uint8_t incoming[SPI_PAGE_SIZE];
    bool in_flight;
    uint32_t pages;
    uint32_t interval_ms;
    uint32_t clocked;
    uint32_t length;
    uint32_t window;
    bool half_duplex;
    uint64_t next_us;
};

#define COUNTER(prop_name, prop_unit) \
    { .name = prop_name, .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RO, \
      .unit = prop_unit, .min = 0, .max = UINT32_MAX }

// CPOL in bit 1, CPHA in bit 0; 8- or 16-bit frames; bit order.
#define FORMAT_PROPERTIES \
    { .name = "mode", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, \
      .unit = NULL, .min = 0, .max = 3 }, \
    { .name = "bits", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, \
      .unit = NULL, .min = 8, .max = 16 }, \
    { .name = "lsb_first", .type = MCLW_TYPE_BOOL, .count = 1, .access = MCLW_ACCESS_RW, \
      .unit = NULL, .min = 0, .max = 1 }

static const struct mclw_property_desc target_properties[] = {
    COUNTER("bytes", "bytes"),
    FORMAT_PROPERTIES,
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
    FORMAT_PROPERTIES,
    {
        .name = "length",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = "bytes",
        .min = 1,
        .max = SPI_PAGE_SIZE,
    },
    {
        .name = "window",
        .type = MCLW_TYPE_U32,
        .count = 1,
        .access = MCLW_ACCESS_RW,
        .unit = NULL,
        .min = 1,
        .max = SPI_PAGE_SIZE,
    },
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

#define PIN(binding_name, binding_kind, binding_required) \
    { .name = binding_name, .kind = binding_kind, .required = binding_required, .usb = NULL }

// MISO is optional: a half-duplex DUT has none.
static const struct mclw_binding_desc target_bindings[] = {
    PIN("sck", MCLW_BINDING_GPIO_IN, true),
    PIN("mosi", MCLW_BINDING_GPIO_IN, true),
    PIN("miso", MCLW_BINDING_GPIO_OUT, false),
    PIN("cs", MCLW_BINDING_GPIO_IN, true),
};

static const struct mclw_binding_desc controller_bindings[] = {
    PIN("sck", MCLW_BINDING_GPIO_OUT, true),
    PIN("mosi", MCLW_BINDING_GPIO_OUT, true),
    PIN("miso", MCLW_BINDING_GPIO_IN, false),
    PIN("cs", MCLW_BINDING_GPIO_OUT, true),
};

static const struct mclw_spi_peer_format default_format = { .mode = 0u, .bits = 8u, .lsb_first = false };

// What each one's partner program sends: spi_slave.c out_buf[i] = ~i,
// spi_master.c out_buf[i] = i.
static void default_pages(struct spi_page_state *s, bool controller) {
    for (size_t i = 0; i < SPI_PAGE_SIZE; i++) {
        uint8_t up = (uint8_t)i;

        s->miso[i] = controller ? 0u : (uint8_t)~up;
        s->mosi[i] = controller ? up : 0u;
    }
}

static struct mclw_gpio *miso_pin(const struct mclw_binding *b) {
    return b[BINDING_MISO].present ? b[BINDING_MISO].gpio : NULL;
}

static uint32_t frame_bytes(const struct spi_page_state *s) {
    return s->format.bits / 8u;
}

static int target_init(struct mclw_instance *inst, const struct mclw_binding *b) {
    struct spi_page_state *s = inst->state;
    int err;

    s->inst = inst;
    s->format = default_format;
    default_pages(s, false);
    s->peer = mclw_hal_spi_peer_target(b[BINDING_SCK].gpio, b[BINDING_MOSI].gpio, miso_pin(b), b[BINDING_CS].gpio,
        &err);
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
    s->format = default_format;
    s->interval_ms = SPI_PAGE_INTERVAL_MS;
    s->length = SPI_PAGE_SIZE;
    s->window = 1u;
    default_pages(s, true);
    s->half_duplex = miso_pin(b) == NULL;
    s->peer = mclw_hal_spi_peer_controller(b[BINDING_SCK].gpio, b[BINDING_MOSI].gpio, miso_pin(b),
        b[BINDING_CS].gpio, SPI_PAGE_HZ, &err);
    return s->peer == NULL ? err : 0;
}

static void page_deinit(struct mclw_instance *inst) {
    struct spi_page_state *s = inst->state;

    mclw_hal_spi_peer_close(s->peer);
    s->peer = NULL;
}

static uint32_t target_bytes(const struct spi_page_state *s) {
    return s->earlier + mclw_hal_spi_peer_count(s->peer) * frame_bytes(s);
}

static int get_format(const struct spi_page_state *s, size_t which, struct mclw_value *out) {
    switch (which) {
        case 0:
            out->v[0] = s->format.mode;
            return 0;
        case 1:
            out->v[0] = s->format.bits;
            return 0;
        default:
            out->v[0] = s->format.lsb_first ? 1 : 0;
            return 0;
    }
}

static int with_format(struct spi_page_state *s, size_t which, int64_t value, struct mclw_spi_peer_format *f) {
    *f = s->format;
    switch (which) {
        case 0:
            f->mode = (uint8_t)value;
            return 0;
        case 1:
            if (value != 8 && value != 16) {
                return -EINVAL;
            }
            f->bits = (uint8_t)value;
            return 0;
        default:
            f->lsb_first = value != 0;
            return 0;
    }
}

static int target_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    const struct spi_page_state *s = inst->state;

    if (property == TARGET_PROP_BYTES) {
        out->v[0] = target_bytes(s);
        return 0;
    }
    if (property >= TARGET_PROP_MODE && property <= TARGET_PROP_LSB_FIRST) {
        return get_format(s, property - TARGET_PROP_MODE, out);
    }
    return -EINVAL;
}

// The rings start again at position 0 in the new format, as after a
// realignment; `bytes` carries on.
static int target_set_format(struct spi_page_state *s, const struct mclw_spi_peer_format *f) {
    uint32_t before = target_bytes(s);
    int err = mclw_hal_spi_peer_set_format(s->peer, f);

    if (err != 0) {
        return err;
    }
    s->format = *f;
    s->earlier = before;
    s->last_count = 0u;
    return 0;
}

static int target_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct spi_page_state *s = inst->state;
    struct mclw_spi_peer_format f;
    int err;

    if (property < TARGET_PROP_MODE || property > TARGET_PROP_LSB_FIRST) {
        return -EINVAL;
    }
    err = with_format(s, property - TARGET_PROP_MODE, in->v[0], &f);
    return err != 0 ? err : target_set_format(s, &f);
}

static int target_reset(struct mclw_instance *inst, bool factory) {
    struct spi_page_state *s = inst->state;

    if (factory) {
        default_pages(s, false);
        if (target_set_format(s, &default_format) != 0) {
            return -EIO;
        }
    }
    s->earlier = 0u;
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

// A new page is served from the next frame, at ring position 0.
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

// A page the DUT clocks whole leaves the count at a page boundary; one that
// stops part way through (a shorter transfer, or a DUT reset under one) is
// over once the count has stood still for SPI_PAGE_REALIGN_US of
// simulation time, and the next page starts at ring position 0. What the
// rings hold stays readable.
static void target_poll(struct mclw_instance *inst) {
    struct spi_page_state *s = inst->state;
    uint32_t count = mclw_hal_spi_peer_count(s->peer);
    uint64_t now = mclw_now_us();

    if ((count * frame_bytes(s)) % SPI_PAGE_SIZE == 0u || count != s->last_count) {
        s->last_count = count;
        s->still_since_us = now;
        return;
    }
    if (now - s->still_since_us >= SPI_PAGE_REALIGN_US) {
        s->earlier += count * frame_bytes(s);
        mclw_hal_spi_peer_restart(s->peer);
        mclw_trace(inst, MCLW_TRACE_STATE, count, 0u, 0);
        s->last_count = 0u;
        s->still_since_us = now;
    }
}

static void controller_settle(struct spi_page_state *s, uint64_t now_us);

static int controller_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    struct spi_page_state *s = inst->state;

    controller_settle(s, mclw_now_us());
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
        case CONTROLLER_PROP_LENGTH:
            out->v[0] = s->length;
            return 0;
        case CONTROLLER_PROP_WINDOW:
            out->v[0] = s->window;
            return 0;
        default:
            if (property >= CONTROLLER_PROP_MODE && property <= CONTROLLER_PROP_LSB_FIRST) {
                return get_format(s, property - CONTROLLER_PROP_MODE, out);
            }
            return -EINVAL;
    }
}

// The first page waits one interval too: a DUT just started gets that long
// to be listening. The format, length and window change only between pages.
static int controller_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    struct spi_page_state *s = inst->state;
    struct mclw_spi_peer_format f;
    int err;

    switch (property) {
        case CONTROLLER_PROP_PAGES:
            s->pages = (uint32_t)in->v[0];
            s->next_us = mclw_now_us() + (uint64_t)s->interval_ms * 1000u;
            return 0;
        case CONTROLLER_PROP_INTERVAL:
            s->interval_ms = (uint32_t)in->v[0];
            return 0;
        case CONTROLLER_PROP_LENGTH:
        case CONTROLLER_PROP_WINDOW:
            if (s->in_flight) {
                return -EBUSY;
            }
            if (property == CONTROLLER_PROP_LENGTH) {
                s->length = (uint32_t)in->v[0];
            } else {
                s->window = (uint32_t)in->v[0];
            }
            return 0;
        default:
            if (property < CONTROLLER_PROP_MODE || property > CONTROLLER_PROP_LSB_FIRST) {
                return -EINVAL;
            }
            err = with_format(s, property - CONTROLLER_PROP_MODE, in->v[0], &f);
            if (err == 0) {
                err = mclw_hal_spi_peer_set_format(s->peer, &f);
            }
            if (err == 0) {
                s->format = f;
            }
            return err;
    }
}

static int controller_reset(struct mclw_instance *inst, bool factory) {
    struct spi_page_state *s = inst->state;

    if (factory) {
        default_pages(s, true);
        s->interval_ms = SPI_PAGE_INTERVAL_MS;
        s->length = SPI_PAGE_SIZE;
        s->window = 1u;
        if (mclw_hal_spi_peer_set_format(s->peer, &default_format) == 0) {
            s->format = default_format;
        }
    }
    s->pages = 0u;
    s->clocked = 0u;
    return 0;
}

static int controller_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf,
    size_t len) {
    struct spi_page_state *s = inst->state;

    controller_settle(s, mclw_now_us());
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

// The page's bytes, whole frames of them.
static uint32_t page_frames(const struct spi_page_state *s) {
    uint32_t frames = s->length / frame_bytes(s);

    return frames != 0u ? frames : 1u;
}

// What has come back so far reaches `miso` as it comes, frame by frame;
// with no MISO, nothing comes back and `miso` reads FFh. A page counts as
// clocked as soon as its last frame is answered, whoever looks first: a
// read of `clocked`, `pages` or `miso` does not wait for the next tick.
static void controller_settle(struct spi_page_state *s, uint64_t now_us) {
    bool busy;

    if (!s->in_flight) {
        return;
    }
    busy = mclw_hal_spi_peer_busy(s->peer);
    if (s->half_duplex) {
        memset(s->miso, 0xff, page_frames(s) * frame_bytes(s));
    } else {
        memcpy(s->miso, s->incoming, page_frames(s) * frame_bytes(s));
    }
    if (busy) {
        return;
    }
    s->in_flight = false;
    s->clocked++;
    s->pages--;
    s->next_us = now_us + (uint64_t)s->interval_ms * 1000u;
    mclw_trace(s->inst, MCLW_TRACE_SPI, s->clocked, (uint16_t)(page_frames(s) * frame_bytes(s)), 0);
}

static void controller_time_advanced(struct mclw_instance *inst, uint64_t now_us) {
    struct spi_page_state *s = inst->state;

    controller_settle(s, now_us);
    if (s->in_flight) {
        return;
    }
    if (s->pages != 0u && now_us >= s->next_us) {
        uint32_t frames = page_frames(s);

        memset(s->incoming, 0xff, sizeof(s->incoming));
        if (mclw_hal_spi_peer_transfer(s->peer, s->mosi, s->incoming, frames, s->window) == 0) {
            s->in_flight = true;
        }
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
