// SPDX-License-Identifier: Apache-2.0 OR MIT
// GPIO for the host port. An output's every change goes to the DUT as a
// bridge action at the current virtual time (host.h); an input reads the
// level the DUT last drove on that pin, which the bridge reports, or the
// pin's pull while the DUT has not driven it. Hardware links (follow,
// join) and square waves are not available here.

#include <errno.h>
#include <stddef.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"

#include "host.h"

#define GPIO_MAX 48u

struct mclw_gpio {
    const void *owner;
    uint8_t pin;
    bool output;
    bool driven;
    bool level;
    enum mclw_gpio_pull pull;
    bool fell;
};

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
    levels_init();
    if (dut_level[pin] != 0 && level == 0 && gpios[pin].owner != NULL) {
        gpios[pin].fell = true;
    }
    dut_level[pin] = (int8_t)level;
    mclw_hal_critical_exit(state);
}

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
    mclw_hal_critical_exit(state);
    if (output) {
        mclw_host_event_gpio(pin, initial ? MCLW_HOST_GPIO_HIGH : MCLW_HOST_GPIO_LOW);
    }
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
    (void)gpio;
    (void)high_ns;
    return period_ns == 0 ? 0 : -ENOTSUP;
}

int mclw_hal_gpio_follow(struct mclw_gpio *to, struct mclw_gpio *from) {
    (void)to;
    (void)from;
    return -ENOTSUP;
}

int mclw_hal_gpio_join(struct mclw_gpio *a, struct mclw_gpio *b) {
    (void)a;
    (void)b;
    return -ENOTSUP;
}

int mclw_hal_gpio_join_clock(struct mclw_gpio *a, struct mclw_gpio *b) {
    (void)a;
    (void)b;
    return -ENOTSUP;
}

void mclw_hal_gpio_unlink(struct mclw_gpio *gpio) {
    (void)gpio;
}
