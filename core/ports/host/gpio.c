// SPDX-License-Identifier: Apache-2.0 OR MIT
// GPIO for the host port. An output's every change goes to the DUT as a
// bridge action at the current virtual time (host.h); an input reads the
// level the DUT last drove on that pin, which the bridge reports, or the
// pin's pull while the DUT has not driven it. A link between two pins
// (follow, join) is a join inside the emulator: the DUT's drive on either
// is the other's input at once, and two blocks routed to joined pins (I2C,
// UART, SPI) talk to each other directly. A square wave is queued as timed
// actions a little ahead of the DUT's time.

#include <errno.h>
#include <stddef.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"

#include "host.h"

#define GPIO_MAX 48u

enum link_kind {
    LINK_NONE,
    LINK_FOLLOW,
    LINK_JOIN,
};

struct mclw_gpio {
    const void *owner;
    uint8_t pin;
    bool output;
    bool driven;
    bool level;
    enum mclw_gpio_pull pull;
    bool fell;
    // A link to another pin: this pin's DUT levels go there.
    uint8_t link;
    struct mclw_gpio *peer;
    // A square wave: period and high time, and when the queued edges end.
    uint32_t period_ns;
    uint32_t high_ns;
    uint64_t square_until_ns;
};

#define SQUARE_AHEAD_NS 2000000u

static struct mclw_gpio gpios[GPIO_MAX];
// The DUT's side of every pin, as the bridge last reported it: 0, 1, or
// unknown (not driven, or never reported).
static int8_t dut_level[GPIO_MAX];
static bool dut_levels_ready;

static void levels_init(void) {
    if (!dut_levels_ready) {
        for (size_t i = 0; i < GPIO_MAX; i++) {
            dut_level[i] = -1;
        }
        dut_levels_ready = true;
    }
}

void mclw_host_gpio_dut(uint8_t pin, int level) {
    if (pin >= GPIO_MAX) {
        return;
    }
    uint32_t state = mclw_hal_critical_enter();
    struct mclw_gpio *g = &gpios[pin];

    levels_init();
    if (dut_level[pin] != 0 && level == 0 && g->owner != NULL) {
        g->fell = true;
    }
    dut_level[pin] = (int8_t)level;
    mclw_hal_critical_exit(state);
}

// The worker's poll: each square wave's edges for the next stretch.
static void square_poll(void *ctx) {
    uint64_t now = mclw_host_event_ns();

    (void)ctx;
    uint32_t state = mclw_hal_critical_enter();
    for (size_t i = 0; i < GPIO_MAX; i++) {
        struct mclw_gpio *g = &gpios[i];

        if (g->owner == NULL || g->period_ns == 0u) {
            continue;
        }
        if (g->square_until_ns < now) {
            g->square_until_ns = now;
        }
        while (g->square_until_ns < now + SQUARE_AHEAD_NS) {
            mclw_host_event_gpio_at(g->pin, MCLW_HOST_GPIO_HIGH, g->square_until_ns);
            mclw_host_event_gpio_at(g->pin, MCLW_HOST_GPIO_LOW, g->square_until_ns + g->high_ns);
            g->square_until_ns += g->period_ns;
        }
    }
    mclw_hal_critical_exit(state);
}

static bool square_registered;

struct mclw_gpio *mclw_hal_gpio_open(uint8_t pin, bool output, enum mclw_gpio_pull pull,
    bool initial, const void *owner, int *err) {
    if (pin >= GPIO_MAX || owner == NULL) {
        *err = -EINVAL;
        return NULL;
    }
    *err = mclw_hal_pin_claim(pin, owner);
    if (*err != 0) {
        return NULL;
    }
    uint32_t state = mclw_hal_critical_enter();
    struct mclw_gpio *g = &gpios[pin];

    levels_init();
    g->owner = owner;
    g->pin = pin;
    g->output = output;
    g->driven = output;
    g->level = initial;
    g->pull = pull;
    g->fell = false;
    g->link = LINK_NONE;
    g->peer = NULL;
    g->period_ns = 0;
    mclw_hal_critical_exit(state);
    if (output) {
        mclw_host_event_gpio(pin, initial ? MCLW_HOST_GPIO_HIGH : MCLW_HOST_GPIO_LOW);
    }
    // Every pin is read too (an output released, an open-drain line), so
    // the emulator reports what the DUT drives on it.
    mclw_host_event_watch(pin);
    return g;
}

void mclw_hal_gpio_close(struct mclw_gpio *gpio) {
    if (gpio == NULL || gpio->owner == NULL) {
        return;
    }
    const void *owner = gpio->owner;

    if (gpio->driven) {
        mclw_host_event_gpio(gpio->pin, MCLW_HOST_GPIO_RELEASED);
    }
    mclw_hal_gpio_unlink(gpio);
    gpio->period_ns = 0;
    gpio->owner = NULL;
    mclw_hal_pin_release(gpio->pin, owner);
}

void mclw_hal_gpio_write(struct mclw_gpio *gpio, bool level) {
    if (gpio->driven && gpio->level == level) {
        return;
    }
    gpio->driven = true;
    gpio->level = level;
    mclw_host_event_gpio(gpio->pin, level ? MCLW_HOST_GPIO_HIGH : MCLW_HOST_GPIO_LOW);
}

void mclw_hal_gpio_release(struct mclw_gpio *gpio) {
    if (!gpio->driven) {
        return;
    }
    gpio->driven = false;
    mclw_host_event_gpio(gpio->pin, MCLW_HOST_GPIO_RELEASED);
}

bool mclw_hal_gpio_read(const struct mclw_gpio *gpio) {
    int8_t dut;
    uint32_t state;

    if (gpio->driven) {
        return gpio->level;
    }
    state = mclw_hal_critical_enter();
    dut = dut_level[gpio->pin];
    mclw_hal_critical_exit(state);
    if (dut >= 0) {
        return dut != 0;
    }
    return gpio->pull != MCLW_GPIO_PULL_DOWN;
}

bool mclw_hal_gpio_fell(struct mclw_gpio *gpio) {
    uint32_t state = mclw_hal_critical_enter();
    bool fell = gpio->fell;

    gpio->fell = false;
    mclw_hal_critical_exit(state);
    return fell;
}

uint8_t mclw_hal_gpio_pin(const struct mclw_gpio *gpio) {
    return gpio->pin;
}

int mclw_hal_gpio_square(struct mclw_gpio *gpio, uint32_t period_ns, uint32_t high_ns) {
    // A high of none or of the whole period is a steady level, as the rp2
    // port's PWM makes it (a model may set the period before its high).
    if (period_ns != 0u && (high_ns == 0u || high_ns >= period_ns)) {
        uint32_t state = mclw_hal_critical_enter();
        gpio->period_ns = 0u;
        mclw_hal_critical_exit(state);
        mclw_hal_gpio_write(gpio, high_ns != 0u);
        return 0;
    }
    if (!square_registered) {
        if (mclw_hal_poll_register(square_poll, NULL) != 0) {
            return -EBUSY;
        }
        square_registered = true;
    }
    uint32_t state = mclw_hal_critical_enter();
    gpio->period_ns = period_ns;
    gpio->high_ns = high_ns;
    gpio->square_until_ns = 0;
    gpio->driven = true;
    mclw_hal_critical_exit(state);
    return 0;
}

static int link(struct mclw_gpio *a, struct mclw_gpio *b, uint8_t kind, bool both) {
    uint32_t state = mclw_hal_critical_enter();

    a->link = kind;
    a->peer = b;
    if (both) {
        b->link = kind;
        b->peer = a;
    }
    mclw_hal_critical_exit(state);
    // A follow is a join too: the pin that follows is the DUT's input.
    mclw_host_event_join(a->pin, b->pin, true);
    return 0;
}

// `from`'s DUT levels go to `to`.
int mclw_hal_gpio_follow(struct mclw_gpio *to, struct mclw_gpio *from) {
    return link(from, to, LINK_FOLLOW, false);
}

int mclw_hal_gpio_join(struct mclw_gpio *a, struct mclw_gpio *b) {
    return link(a, b, LINK_JOIN, true);
}

// An I2C clock's stretching is the DUT controller's own here: the bridge
// carries levels, so a clock join is a join.
int mclw_hal_gpio_join_clock(struct mclw_gpio *a, struct mclw_gpio *b) {
    return link(a, b, LINK_JOIN, true);
}

void mclw_hal_gpio_unlink(struct mclw_gpio *gpio) {
    uint32_t state = mclw_hal_critical_enter();
    struct mclw_gpio *peer = gpio->peer;

    if (peer != NULL && peer->peer == gpio) {
        peer->link = LINK_NONE;
        peer->peer = NULL;
    }
    gpio->link = LINK_NONE;
    gpio->peer = NULL;
    mclw_hal_critical_exit(state);
    if (peer != NULL) {
        mclw_host_event_join(gpio->pin, peer->pin, false);
    }
}
