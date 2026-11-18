// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/pins.h"

#include "gpio_internal.h"

#ifndef MCLW_GPIO_MAX
#define MCLW_GPIO_MAX 16
#endif

#define NS_PER_S 1000000000ull
// The PWM counter wraps at TOP + 1 and a channel is high while the counter
// is below its level, so a level of TOP + 1 holds it high for the whole
// period (pico-examples' pwm/measure_duty_cycle drives 100 % that way). TOP
// stays below 0xffff to keep that level in the 16-bit compare register.
#define PWM_COUNTS_MAX 0xffffu
#define PWM_DIV_MAX 255u

static struct mclw_gpio slots[MCLW_GPIO_MAX];

// Both channels of a slice share its divider and wrap, so a second pin on
// a slice takes the period the first one set.
static struct {
    uint8_t users;
    uint8_t div;
    uint16_t top;
} slices[NUM_PWM_SLICES];

struct mclw_gpio *mclw_hal_gpio_open(uint8_t pin, bool output, enum mclw_gpio_pull pull,
    bool initial, const void *owner, int *err) {
    struct mclw_gpio *g = NULL;
    for (size_t i = 0; i < MCLW_GPIO_MAX; i++) {
        if (slots[i].owner == NULL) {
            g = &slots[i];
            break;
        }
    }
    if (g == NULL) {
        *err = -ENOSPC;
        return NULL;
    }
    *err = mclw_hal_pin_claim(pin, owner);
    if (*err != 0) {
        return NULL;
    }
    g->pin = pin;
    g->output = output;
    g->square = false;
    g->pull = pull;
    g->link = NULL;
    g->held = false;
    g->owner = owner;

    gpio_init(pin);
    gpio_set_pulls(pin, pull == MCLW_GPIO_PULL_UP, pull == MCLW_GPIO_PULL_DOWN);
    if (output) {
        gpio_put(pin, initial);
        gpio_set_dir(pin, GPIO_OUT);
    } else {
        gpio_set_dir(pin, GPIO_IN);
        gpio_acknowledge_irq(pin, GPIO_IRQ_EDGE_FALL);
    }
    return g;
}

void mclw_rp2_gpio_restore(struct mclw_gpio *g) {
    gpio_set_pulls(g->pin, g->pull == MCLW_GPIO_PULL_UP, g->pull == MCLW_GPIO_PULL_DOWN);
    gpio_set_dir(g->pin, g->output);
    gpio_set_function(g->pin, GPIO_FUNC_SIO);
}

static void square_stop(struct mclw_gpio *g) {
    uint slice = pwm_gpio_to_slice_num(g->pin);

    if (!g->square) {
        return;
    }
    g->square = false;
    slices[slice].users &= (uint8_t)~(1u << pwm_gpio_to_channel(g->pin));
    if (slices[slice].users == 0u) {
        pwm_set_enabled(slice, false);
    }
    mclw_rp2_gpio_restore(g);
}

void mclw_hal_gpio_close(struct mclw_gpio *g) {
    if (g == NULL || g->owner == NULL) {
        return;
    }
    mclw_hal_gpio_unlink(g);
    square_stop(g);
    gpio_deinit(g->pin);
    mclw_hal_pin_release(g->pin, g->owner);
    g->owner = NULL;
}

void mclw_hal_gpio_write(struct mclw_gpio *g, bool level) {
    if (g->output) {
        gpio_put(g->pin, level);
        gpio_set_dir(g->pin, GPIO_OUT);
    }
}

void mclw_hal_gpio_release(struct mclw_gpio *g) {
    if (g->output) {
        gpio_set_dir(g->pin, GPIO_IN);
    }
}

bool mclw_hal_gpio_read(const struct mclw_gpio *g) {
    return gpio_get(g->pin);
}

// The bank's raw edge latch holds a fall whether or not any interrupt is
// enabled for the pin. MicroPython's GPIO interrupt handler acknowledges
// every pin's latches when a Pin.irq fires, so with one in use a fall can
// still go unseen.
bool mclw_hal_gpio_fell(struct mclw_gpio *g) {
    uint32_t events = io_bank0_hw->intr[g->pin / 8u] >> (4u * (g->pin % 8u));

    if ((events & GPIO_IRQ_EDGE_FALL) == 0u) {
        return false;
    }
    gpio_acknowledge_irq(g->pin, GPIO_IRQ_EDGE_FALL);
    return true;
}

uint8_t mclw_hal_gpio_pin(const struct mclw_gpio *g) {
    return g->pin;
}

// An integer divider only: a fractional one dithers the period from one
// cycle to the next, which a part clocked by the wave would see as jitter.
int mclw_hal_gpio_square(struct mclw_gpio *g, uint32_t period_ns, uint32_t high_ns) {
    uint64_t hz = clock_get_hz(clk_sys);
    uint64_t cycles;
    uint64_t div;
    uint64_t counts;
    uint64_t level;
    uint slice;
    uint channel;
    uint8_t others;

    if (g == NULL || !g->output || g->link != NULL || g->held) {
        return -EINVAL;
    }
    if (period_ns == 0u) {
        square_stop(g);
        return 0;
    }
    cycles = ((uint64_t)period_ns * hz + NS_PER_S / 2u) / NS_PER_S;
    div = (cycles + PWM_COUNTS_MAX - 1u) / PWM_COUNTS_MAX;
    if (cycles < 2u || div > PWM_DIV_MAX) {
        return -ERANGE;
    }
    counts = (cycles + div / 2u) / div;
    if (counts > PWM_COUNTS_MAX) {
        counts = PWM_COUNTS_MAX;
    }
    level = high_ns >= period_ns ? counts :
        ((uint64_t)high_ns * hz / div + NS_PER_S / 2u) / NS_PER_S;
    if (level > counts) {
        level = counts;
    }

    slice = pwm_gpio_to_slice_num(g->pin);
    channel = pwm_gpio_to_channel(g->pin);
    others = (uint8_t)(slices[slice].users & ~(1u << channel));
    if (others != 0u && (slices[slice].div != div || slices[slice].top != counts - 1u)) {
        return -EBUSY;
    }
    if (others == 0u) {
        pwm_set_enabled(slice, false);
        pwm_set_clkdiv_int_frac4(slice, (uint8_t)div, 0u);
        pwm_set_wrap(slice, (uint16_t)(counts - 1u));
        pwm_set_counter(slice, 0u);
        slices[slice].div = (uint8_t)div;
        slices[slice].top = (uint16_t)(counts - 1u);
    }
    pwm_set_chan_level(slice, channel, (uint16_t)level);
    slices[slice].users |= (uint8_t)(1u << channel);
    pwm_set_enabled(slice, true);
    gpio_set_function(g->pin, GPIO_FUNC_PWM);
    g->square = true;
    return 0;
}
