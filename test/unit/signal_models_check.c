// SPDX-License-Identifier: Apache-2.0 OR MIT
// Runs square_wave, jumper, jumper_open_drain, spi_page_target and
// spi_page_controller on the host behind stand-ins for the gpio and SPI
// peer HAL, with a simulated clock ticking time_advanced and poll once a
// millisecond, and checks what each asks of the HAL: the edges a wave on
// the clock makes, the square wave it hands the hardware, the links, the
// pages. Prints "ok".

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/spi_peer.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/value.h"

#include "jumper.h"
#include "spi_page.h"
#include "square_wave.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("failed: %s (line %d)\n", #cond, __LINE__); \
            exit(1); \
        } \
    } while (0)

#define EDGES_MAX 64u

struct mclw_gpio {
    uint8_t pin;
    bool level;
    uint32_t square_period;
    uint32_t square_high;
    struct mclw_gpio *linked;
    int link_kind;
    size_t edges;
    uint64_t edge_us[EDGES_MAX];
};

// Like the rp2 port: an integer divider of 125 MHz up to 255 over a
// 16-bit counter, so 16 ns to about 134 ms.
#define SQUARE_MIN_NS 16u
#define SQUARE_MAX_NS 133693440u

static struct mclw_gpio pins[4];
static struct mclw_instance instance;
static uint64_t now_us;

uint64_t mclw_now_us(void) {
    return now_us;
}

void mclw_trace(struct mclw_instance *inst, enum mclw_trace_kind kind, uint32_t addr, uint16_t len,
    int16_t result) {
    (void)inst;
    (void)kind;
    (void)addr;
    (void)len;
    (void)result;
}

void mclw_hal_gpio_write(struct mclw_gpio *g, bool level) {
    if (level != g->level && g->edges < EDGES_MAX) {
        g->edge_us[g->edges++] = now_us;
    }
    g->level = level;
}

uint8_t mclw_hal_gpio_pin(const struct mclw_gpio *g) {
    return g->pin;
}

int mclw_hal_gpio_square(struct mclw_gpio *g, uint32_t period_ns, uint32_t high_ns) {
    if (period_ns != 0u && (period_ns < SQUARE_MIN_NS || period_ns > SQUARE_MAX_NS)) {
        return -ERANGE;
    }
    g->square_period = period_ns;
    g->square_high = high_ns;
    return 0;
}

int mclw_hal_gpio_follow(struct mclw_gpio *to, struct mclw_gpio *from) {
    if (to->linked != NULL || from->linked != NULL) {
        return -EINVAL;
    }
    to->linked = from;
    from->linked = to;
    to->link_kind = 1;
    from->link_kind = -1;
    return 0;
}

int mclw_hal_gpio_join(struct mclw_gpio *a, struct mclw_gpio *b) {
    if (a->linked != NULL || b->linked != NULL) {
        return -EINVAL;
    }
    a->linked = b;
    b->linked = a;
    a->link_kind = 2;
    b->link_kind = 2;
    return 0;
}

void mclw_hal_gpio_unlink(struct mclw_gpio *g) {
    if (g->linked != NULL) {
        g->linked->linked = NULL;
        g->linked->link_kind = 0;
        g->linked = NULL;
        g->link_kind = 0;
    }
}

// The SPI peer: a target's rings and count, a controller's transfer.
struct mclw_spi_peer {
    bool controller;
    bool open;
    uint8_t tx[MCLW_SPI_PEER_RING];
    uint8_t rx[MCLW_SPI_PEER_RING];
    uint32_t count;
    const uint8_t *out;
    uint8_t *in;
    size_t len;
    bool busy;
    uint32_t restarts;
};

static struct mclw_spi_peer peer;

static struct mclw_spi_peer *open_peer(bool controller, struct mclw_gpio *const *p, int *err) {
    CHECK(p[0] == &pins[0] && p[1] == &pins[1] && p[2] == &pins[2] && p[3] == &pins[3]);
    if (peer.open) {
        *err = -EBUSY;
        return NULL;
    }
    memset(&peer, 0, sizeof(peer));
    peer.controller = controller;
    peer.open = true;
    return &peer;
}

struct mclw_spi_peer *mclw_hal_spi_peer_target(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, int *err) {
    struct mclw_gpio *const p[4] = { sck, mosi, miso, cs };
    return open_peer(false, p, err);
}

struct mclw_spi_peer *mclw_hal_spi_peer_controller(struct mclw_gpio *sck, struct mclw_gpio *mosi,
    struct mclw_gpio *miso, struct mclw_gpio *cs, uint32_t hz, int *err) {
    struct mclw_gpio *const p[4] = { sck, mosi, miso, cs };
    CHECK(hz == SPI_PAGE_HZ);
    return open_peer(true, p, err);
}

void mclw_hal_spi_peer_close(struct mclw_spi_peer *p) {
    p->open = false;
}

void mclw_hal_spi_peer_restart(struct mclw_spi_peer *p) {
    p->count = 0u;
    p->restarts++;
}

void mclw_hal_spi_peer_load(struct mclw_spi_peer *p, const uint8_t *bytes, size_t len) {
    memset(p->tx, 0, sizeof(p->tx));
    memcpy(p->tx, bytes, len);
    mclw_hal_spi_peer_restart(p);
}

void mclw_hal_spi_peer_received(const struct mclw_spi_peer *p, size_t at, uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        bytes[i] = p->rx[(at + i) % MCLW_SPI_PEER_RING];
    }
}

int mclw_hal_spi_peer_transfer(struct mclw_spi_peer *p, const uint8_t *out, uint8_t *in, size_t len) {
    if (p->busy) {
        return -EBUSY;
    }
    p->out = out;
    p->in = in;
    p->len = len;
    p->busy = true;
    return 0;
}

bool mclw_hal_spi_peer_busy(const struct mclw_spi_peer *p) {
    return p->busy;
}

uint32_t mclw_hal_spi_peer_count(const struct mclw_spi_peer *p) {
    return p->count;
}

// The DUT as a controller clocking `n` windows into the target.
static void dut_clocks(size_t n, uint8_t (*mosi)(size_t), uint8_t *miso) {
    for (size_t i = 0; i < n; i++) {
        size_t at = peer.count % MCLW_SPI_PEER_RING;
        if (miso != NULL) {
            miso[i] = peer.tx[at];
        }
        peer.rx[at] = mosi(i);
        peer.count++;
    }
}

static uint8_t counting(size_t i) {
    return (uint8_t)i;
}

static uint8_t junk(size_t i) {
    return (uint8_t)(0xa5u ^ i);
}

static void tick(const struct mclw_model *model, uint64_t ms) {
    for (uint64_t i = 0; i < ms; i++) {
        now_us += 1000u;
        if (model->ops->time_advanced != NULL) {
            model->ops->time_advanced(&instance, now_us);
        }
        if (model->ops->poll != NULL) {
            model->ops->poll(&instance);
        }
    }
}

static void attach(const struct mclw_model *model) {
    static uint8_t state[4096];
    struct mclw_binding bindings[4];

    CHECK(model->state_size <= sizeof(state));
    memset(state, 0, sizeof(state));
    memset(pins, 0, sizeof(pins));
    memset(&instance, 0, sizeof(instance));
    memset(bindings, 0, sizeof(bindings));
    instance.model = model;
    instance.state = state;
    for (size_t i = 0; i < model->binding_count; i++) {
        pins[i].pin = (uint8_t)(10u + i);
        bindings[i].kind = model->bindings[i].kind;
        bindings[i].present = true;
        bindings[i].gpio = &pins[i];
    }
    CHECK(model->ops->init(&instance, bindings) == 0);
}

static int64_t get(const struct mclw_model *model, const char *name) {
    struct mclw_value v = { .count = 1 };
    for (size_t i = 0; i < model->property_count; i++) {
        if (strcmp(model->properties[i].name, name) == 0) {
            CHECK(model->ops->get(&instance, i, &v) == 0);
            return v.v[0];
        }
    }
    CHECK(!"no such property");
    return 0;
}

static int set(const struct mclw_model *model, const char *name, int64_t value) {
    struct mclw_value v = { .count = 1, .v = { value } };
    for (size_t i = 0; i < model->property_count; i++) {
        if (strcmp(model->properties[i].name, name) == 0) {
            return model->ops->set(&instance, i, &v);
        }
    }
    CHECK(!"no such property");
    return -EINVAL;
}

static void square_wave(void) {
    const struct mclw_model *m = &mclw_model_square_wave;
    struct mclw_gpio *out = &pins[0];

    now_us = 5000u;
    attach(m);
    CHECK(!out->level && out->edges == 0u && get(m, "hardware") == 0);

    // GP15's wake wave: 1 s, high for 900 ms, from the moment it is set.
    CHECK(set(m, "period", 1000000000) == 0);
    CHECK(set(m, "high", 900000000) == 0);
    uint64_t start = now_us;
    CHECK(out->level && out->edges == 1u);
    tick(m, 3000);
    // Down at 900 ms, up at 1000, down at 1900, ..., up at 3000: six more.
    CHECK(out->edges == 7u);
    for (size_t i = 1; i < out->edges; i++) {
        uint64_t want = start + (i % 2 ? 900000u : 0u) + 1000000u * (i / 2);
        CHECK(out->edge_us[i] == want);
    }
    CHECK(out->square_period == 0u && get(m, "hardware") == 0);

    // 46.875 kHz goes to the hardware, and back off again.
    CHECK(set(m, "period", 21333) == 0);
    CHECK(set(m, "high", 10667) == 0);
    CHECK(out->square_period == 21333u && out->square_high == 10667u && get(m, "hardware") == 1);
    size_t edges = out->edges;
    tick(m, 10);
    CHECK(out->edges == edges);
    // A period the hardware cannot make is refused and nothing changes.
    CHECK(set(m, "period", 5) == -ERANGE);
    CHECK(get(m, "period") == 21333 && out->square_period == 21333u);

    // A steady level, then period 0 with it high.
    CHECK(set(m, "period", 0) == 0);
    CHECK(out->square_period == 0u && get(m, "hardware") == 0);
    CHECK(!out->level);
    CHECK(set(m, "level", 1) == 0);
    CHECK(out->level);
    tick(m, 100);
    CHECK(out->level);
    // A wave longer than the hardware's longest still follows the clock.
    CHECK(set(m, "period", 4000000000u) == 0);
    CHECK(set(m, "high", 2000000000u) == 0);
    CHECK(out->square_period == 0u && get(m, "hardware") == 0);

    CHECK(m->ops->reset(&instance, true) == 0);
    CHECK(get(m, "period") == 0 && !out->level);
    m->ops->deinit(&instance);
}

static void jumpers(void) {
    attach(&mclw_model_jumper);
    CHECK(pins[1].linked == &pins[0] && pins[1].link_kind == 1 && pins[0].link_kind == -1);
    CHECK(get(&mclw_model_jumper, "linked") == 1);
    mclw_model_jumper.ops->deinit(&instance);
    CHECK(pins[0].linked == NULL && pins[1].linked == NULL);

    attach(&mclw_model_jumper_open_drain);
    CHECK(pins[0].linked == &pins[1] && pins[0].link_kind == 2 && pins[1].link_kind == 2);
    CHECK(mclw_model_jumper_open_drain.bindings[0].kind == MCLW_BINDING_GPIO_IN);
    CHECK(mclw_model_jumper_open_drain.bindings[1].kind == MCLW_BINDING_GPIO_IN);
    mclw_model_jumper_open_drain.ops->deinit(&instance);
    CHECK(pins[0].linked == NULL);
}

static void page_target(void) {
    const struct mclw_model *m = &mclw_model_spi_page_target;
    uint8_t miso[SPI_PAGE_SIZE];
    uint8_t got[SPI_PAGE_SIZE];

    now_us = 0u;
    attach(m);
    CHECK(m->bindings[2].kind == MCLW_BINDING_GPIO_OUT && m->bindings[0].kind == MCLW_BINDING_GPIO_IN);
    // spi_master's page: 00..ff out, the slave's ~i back.
    dut_clocks(SPI_PAGE_SIZE, counting, miso);
    for (size_t i = 0; i < SPI_PAGE_SIZE; i++) {
        CHECK(miso[i] == (uint8_t)~i);
    }
    CHECK(m->ops->read_region(&instance, 1, 0, got, sizeof(got)) == 0);
    for (size_t i = 0; i < SPI_PAGE_SIZE; i++) {
        CHECK(got[i] == (uint8_t)i);
    }
    tick(m, 20);
    CHECK(get(m, "bytes") == 256 && peer.restarts == 1u);

    // A page cut short by a DUT reset: after a few quiet polls the rings
    // start again at 0, and the next page lines up.
    dut_clocks(100, junk, NULL);
    tick(m, SPI_PAGE_REALIGN_POLLS);
    CHECK(peer.count == 356u);
    tick(m, 1);
    CHECK(peer.count == 0u && get(m, "bytes") == 356);
    dut_clocks(SPI_PAGE_SIZE, counting, miso);
    CHECK(miso[0] == 0xffu && miso[255] == 0x00u);
    tick(m, 20);
    CHECK(peer.count == 256u);

    // A new answer from the next window on.
    uint8_t page[2] = { 0x12, 0x34 };
    CHECK(m->ops->write_region(&instance, 0, 0, page, sizeof(page)) == 0);
    dut_clocks(2, counting, miso);
    CHECK(miso[0] == 0x12 && miso[1] == 0x34);
    CHECK(get(m, "bytes") == 614);
    m->ops->deinit(&instance);
    CHECK(!peer.open);
}

static void page_controller(void) {
    const struct mclw_model *m = &mclw_model_spi_page_controller;
    uint8_t got[SPI_PAGE_SIZE];

    now_us = 0u;
    attach(m);
    CHECK(m->bindings[2].kind == MCLW_BINDING_GPIO_IN && m->bindings[0].kind == MCLW_BINDING_GPIO_OUT);
    tick(m, 2000);
    CHECK(!peer.busy && get(m, "clocked") == 0);

    CHECK(set(m, "pages", 2) == 0);
    tick(m, SPI_PAGE_INTERVAL_MS - 1u);
    CHECK(!peer.busy);
    tick(m, 1);
    CHECK(peer.busy && peer.len == SPI_PAGE_SIZE);
    for (size_t i = 0; i < SPI_PAGE_SIZE; i++) {
        CHECK(peer.out[i] == (uint8_t)i);
        peer.in[i] = (uint8_t)~i;
    }
    tick(m, 5);
    peer.busy = false;
    tick(m, 1);
    CHECK(get(m, "clocked") == 1 && get(m, "pages") == 1);
    CHECK(m->ops->read_region(&instance, 0, 0, got, sizeof(got)) == 0);
    for (size_t i = 0; i < SPI_PAGE_SIZE; i++) {
        CHECK(got[i] == (uint8_t)~i);
    }
    tick(m, SPI_PAGE_INTERVAL_MS - 1u);
    CHECK(!peer.busy);
    tick(m, 1);
    CHECK(peer.busy);
    peer.busy = false;
    tick(m, 1);
    CHECK(get(m, "clocked") == 2 && get(m, "pages") == 0);
    tick(m, 3 * SPI_PAGE_INTERVAL_MS);
    CHECK(!peer.busy && get(m, "clocked") == 2);
    m->ops->deinit(&instance);
}

int main(void) {
    square_wave();
    jumpers();
    page_target();
    page_controller();
    puts("ok");
    return 0;
}
