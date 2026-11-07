// SPDX-License-Identifier: Apache-2.0 OR MIT
// Copyright (c) 2021-2023 Raspberry Pi (Trading) Ltd.
// PIO program words and transfer sequencing adapted from Raspberry Pi's
// Debug Probe firmware (MIT).

#include <errno.h>
#include <stddef.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/time.h"

#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/swd.h"
#include "pio_window.h"

#define PROBE_TIMEOUT_US 20000u

#define PROBE_MIN_CLOCK_HZ 50000u
#define PROBE_DEFAULT_CLOCK_HZ 1000000u

#define PROBE_REQUEST_RNW (1u << 1)

#define PROBE_WRAP_TARGET 3u
#define PROBE_WRAP 10u
#define PROBE_WRITE_CMD 0u
#define PROBE_GET_NEXT_CMD 3u
#define PROBE_READ_CMD 8u
#define PROBE_PROGRAM_LENGTH 11u

enum probe_command {
    PROBE_CMD_WRITE,
    PROBE_CMD_SKIP,
    PROBE_CMD_READ,
};

struct mclw_swd {
    const void *owner;
    PIO pio;
    uint8_t sm;
    uint8_t offset;
    uint8_t swclk;
    uint8_t swdio;
    uint8_t nreset;
    uint8_t turnaround;
    bool data_phase;
    bool active;
    uint32_t request_hz;
    uint32_t clock_hz;
};

static const uint16_t probe_instructions[] = {
    0x80a0u, 0x7101u, 0x1941u, 0x90a0u,
    0x6028u, 0x6081u, 0x60a5u, 0xa042u,
    0x5901u, 0x1047u, 0x8020u,
};

static const pio_program_t probe_program = {
    .instructions = probe_instructions,
    .length = PROBE_PROGRAM_LENGTH,
    .origin = -1,
    .pio_version = 0,
};

static struct mclw_swd slot;

static uint32_t probe_min(uint32_t left, uint32_t right) {
    return left < right ? left : right;
}

static uint32_t probe_command_word(const struct mclw_swd *swd, uint32_t count, bool output,
    enum probe_command command) {
    uint32_t address;

    switch (command) {
        case PROBE_CMD_WRITE:
            address = (uint32_t)swd->offset + PROBE_WRITE_CMD;
            break;
        case PROBE_CMD_READ:
            address = (uint32_t)swd->offset + PROBE_READ_CMD;
            break;
        case PROBE_CMD_SKIP:
        default:
            address = (uint32_t)swd->offset + PROBE_GET_NEXT_CMD;
            break;
    }
    return ((count - 1u) & 0xffu) | ((uint32_t)output << 8) | (address << 9);
}

static void probe_resync(struct mclw_swd *swd) {
    const uint32_t debug_bits =
        (1u << (PIO_FDEBUG_TXSTALL_LSB + swd->sm)) |
        (1u << (PIO_FDEBUG_TXOVER_LSB + swd->sm)) |
        (1u << (PIO_FDEBUG_RXUNDER_LSB + swd->sm)) |
        (1u << (PIO_FDEBUG_RXSTALL_LSB + swd->sm));

    pio_sm_set_enabled(swd->pio, swd->sm, false);
    pio_sm_clear_fifos(swd->pio, swd->sm);
    pio_sm_restart(swd->pio, swd->sm);
    pio_sm_clkdiv_restart(swd->pio, swd->sm);
    swd->pio->fdebug = debug_bits;
    pio_sm_exec(swd->pio, swd->sm, pio_encode_jmp((uint)swd->offset + PROBE_GET_NEXT_CMD));
    pio_sm_set_enabled(swd->pio, swd->sm, true);
}

static int probe_put(struct mclw_swd *swd, uint32_t value) {
    absolute_time_t deadline = make_timeout_time_us(PROBE_TIMEOUT_US);

    while (pio_sm_is_tx_fifo_full(swd->pio, swd->sm)) {
        if (time_reached(deadline)) {
            probe_resync(swd);
            return -ETIMEDOUT;
        }
        tight_loop_contents();
    }
    pio_sm_put(swd->pio, swd->sm, value);
    return 0;
}

static int probe_get(struct mclw_swd *swd, uint32_t *value) {
    absolute_time_t deadline = make_timeout_time_us(PROBE_TIMEOUT_US);

    while (pio_sm_is_rx_fifo_empty(swd->pio, swd->sm)) {
        if (time_reached(deadline)) {
            probe_resync(swd);
            return -ETIMEDOUT;
        }
        tight_loop_contents();
    }
    *value = pio_sm_get(swd->pio, swd->sm);
    return 0;
}

static int probe_wait_idle(struct mclw_swd *swd) {
    const uint32_t stall = 1u << (PIO_FDEBUG_TXSTALL_LSB + swd->sm);
    absolute_time_t deadline;

    swd->pio->fdebug = stall;
    deadline = make_timeout_time_us(PROBE_TIMEOUT_US);
    while ((swd->pio->fdebug & stall) == 0u) {
        if (time_reached(deadline)) {
            probe_resync(swd);
            return -ETIMEDOUT;
        }
        tight_loop_contents();
    }
    return 0;
}

static int probe_write(struct mclw_swd *swd, uint32_t count, uint32_t value) {
    int ret = probe_put(swd, probe_command_word(swd, count, true, PROBE_CMD_WRITE));

    return ret != 0 ? ret : probe_put(swd, value);
}

static int probe_hiz(struct mclw_swd *swd, uint32_t count) {
    int ret = probe_put(swd, probe_command_word(swd, count, false, PROBE_CMD_WRITE));

    return ret != 0 ? ret : probe_put(swd, 0u);
}

static int probe_read(struct mclw_swd *swd, uint32_t count, uint32_t *value) {
    int ret = probe_put(swd, probe_command_word(swd, count, false, PROBE_CMD_READ));

    if (ret != 0) {
        return ret;
    }
    ret = probe_get(swd, value);
    if (ret != 0) {
        return ret;
    }
    if (count < 32u) {
        *value >>= 32u - count;
    }
    return 0;
}

static void probe_apply_clock(struct mclw_swd *swd) {
    const uint64_t numerator = (uint64_t)clock_get_hz(clk_sys) * 256u;
    const uint64_t denominator = (uint64_t)swd->request_hz * 4u;
    uint64_t divider256 = (numerator + denominator - 1u) / denominator;

    if (divider256 < 256u) {
        divider256 = 256u;
    }
    if (divider256 > (uint64_t)0xffffu * 256u + 255u) {
        divider256 = (uint64_t)0xffffu * 256u + 255u;
    }
    swd->clock_hz = (uint32_t)(numerator / (divider256 * 4u));
    pio_sm_set_clkdiv_int_frac8(swd->pio, swd->sm, (uint32_t)(divider256 >> 8),
        (uint8_t)(divider256 & 0xffu));
}

static void probe_reset_configure(const struct mclw_swd *swd) {
    if (swd->nreset == MCLW_PIN_NONE) {
        return;
    }
    gpio_init(swd->nreset);
    gpio_put(swd->nreset, 0);
    gpio_set_dir(swd->nreset, GPIO_IN);
    gpio_pull_up(swd->nreset);
}

static void probe_reset_set(const struct mclw_swd *swd, bool asserted) {
    if (swd->nreset == MCLW_PIN_NONE) {
        return;
    }
    if (asserted) {
        gpio_put(swd->nreset, 0);
        gpio_set_dir(swd->nreset, GPIO_OUT);
    } else {
        gpio_set_dir(swd->nreset, GPIO_IN);
        gpio_pull_up(swd->nreset);
    }
}

static bool probe_reset_get(const struct mclw_swd *swd) {
    return swd->nreset == MCLW_PIN_NONE ? true : gpio_get(swd->nreset);
}

static void probe_release_pads(const struct mclw_swd *swd) {
    gpio_set_function(swd->swclk, GPIO_FUNC_SIO);
    gpio_set_function(swd->swdio, GPIO_FUNC_SIO);
    gpio_set_dir(swd->swclk, GPIO_IN);
    gpio_set_dir(swd->swdio, GPIO_IN);
    gpio_disable_pulls(swd->swclk);
    gpio_disable_pulls(swd->swdio);
}

static int probe_acquire_sm(struct mclw_swd *swd) {
    int err = -ENOMEM;

    for (uint32_t i = 0; i < NUM_PIOS; i++) {
        PIO pio = pio_get_instance(i);
        int sm;

        if (!pio_can_add_program(pio, &probe_program) ||
            !mclw_pio_window(pio, MCLW_PIO_PIN(swd->swclk) | MCLW_PIO_PIN(swd->swdio))) {
            continue;
        }
        sm = pio_claim_unused_sm(pio, false);
        if (sm < 0) {
            err = -EBUSY;
            continue;
        }
        swd->pio = pio;
        swd->sm = (uint8_t)sm;
        swd->offset = (uint8_t)pio_add_program(pio, &probe_program);
        return 0;
    }
    return err;
}

static void probe_release_sm(struct mclw_swd *swd) {
    pio_remove_program(swd->pio, &probe_program, swd->offset);
    pio_sm_unclaim(swd->pio, swd->sm);
}

struct mclw_swd *mclw_hal_swd_open(uint8_t swclk, uint8_t swdio, uint8_t nreset,
    const void *owner, int *err) {
    struct mclw_swd *swd = &slot;

    if (swd->owner != NULL) {
        *err = -EBUSY;
        return NULL;
    }
    if (owner == NULL || swclk == swdio ||
        (nreset != MCLW_PIN_NONE && (nreset == swclk || nreset == swdio))) {
        *err = -EINVAL;
        return NULL;
    }

    *err = mclw_hal_pin_claim(swclk, owner);
    if (*err != 0) {
        return NULL;
    }
    *err = mclw_hal_pin_claim(swdio, owner);
    if (*err != 0) {
        mclw_hal_pin_release(swclk, owner);
        return NULL;
    }
    if (nreset != MCLW_PIN_NONE) {
        *err = mclw_hal_pin_claim(nreset, owner);
        if (*err != 0) {
            mclw_hal_pin_release(swdio, owner);
            mclw_hal_pin_release(swclk, owner);
            return NULL;
        }
    }

    swd->swclk = swclk;
    swd->swdio = swdio;
    swd->nreset = nreset;
    swd->turnaround = 1;
    swd->data_phase = false;
    swd->active = false;
    swd->request_hz = PROBE_DEFAULT_CLOCK_HZ;
    swd->clock_hz = PROBE_DEFAULT_CLOCK_HZ;

    *err = probe_acquire_sm(swd);
    if (*err != 0) {
        if (nreset != MCLW_PIN_NONE) {
            mclw_hal_pin_release(nreset, owner);
        }
        mclw_hal_pin_release(swdio, owner);
        mclw_hal_pin_release(swclk, owner);
        return NULL;
    }

    probe_release_pads(swd);
    probe_reset_configure(swd);
    swd->owner = owner;
    return swd;
}

void mclw_hal_swd_close(struct mclw_swd *swd) {
    if (swd == NULL || swd->owner == NULL) {
        return;
    }
    (void)mclw_hal_swd_port_off(swd);
    probe_release_sm(swd);
    mclw_hal_pin_release(swd->swclk, swd->owner);
    mclw_hal_pin_release(swd->swdio, swd->owner);
    if (swd->nreset != MCLW_PIN_NONE) {
        mclw_hal_pin_release(swd->nreset, swd->owner);
    }
    swd->owner = NULL;
}

int mclw_hal_swd_port_on(struct mclw_swd *swd) {
    pio_sm_config config = pio_get_default_sm_config();
    uint64_t pin_mask;
    int ret;

    if (swd == NULL || swd->owner == NULL) {
        return -EINVAL;
    }
    if (swd->active) {
        probe_resync(swd);
        return 0;
    }
    probe_reset_configure(swd);
    pio_gpio_init(swd->pio, swd->swclk);
    pio_gpio_init(swd->pio, swd->swdio);
    gpio_pull_up(swd->swdio);

    sm_config_set_wrap(&config, (uint)swd->offset + PROBE_WRAP_TARGET,
        (uint)swd->offset + PROBE_WRAP);
    sm_config_set_sideset(&config, 2u, true, false);
    sm_config_set_sideset_pins(&config, swd->swclk);
    sm_config_set_out_pins(&config, swd->swdio, 1u);
    sm_config_set_set_pins(&config, swd->swdio, 1u);
    sm_config_set_in_pins(&config, swd->swdio);
    sm_config_set_out_shift(&config, true, false, 0u);
    sm_config_set_in_shift(&config, true, false, 0u);

    pin_mask = MCLW_PIO_PIN(swd->swclk) | MCLW_PIO_PIN(swd->swdio);
    pio_sm_set_pindirs_with_mask64(swd->pio, swd->sm, pin_mask, pin_mask);
    ret = pio_sm_init(swd->pio, swd->sm, (uint)swd->offset, &config);
    if (ret != 0) {
        probe_release_pads(swd);
        return -EIO;
    }
    probe_apply_clock(swd);
    pio_sm_exec(swd->pio, swd->sm, pio_encode_jmp((uint)swd->offset + PROBE_GET_NEXT_CMD));
    pio_sm_set_enabled(swd->pio, swd->sm, true);
    swd->active = true;
    return 0;
}

int mclw_hal_swd_port_off(struct mclw_swd *swd) {
    int ret = 0;

    if (swd == NULL || swd->owner == NULL) {
        return -EINVAL;
    }
    if (swd->active) {
        ret = probe_put(swd, probe_command_word(swd, 0u, false, PROBE_CMD_SKIP));
        if (ret == 0) {
            ret = probe_wait_idle(swd);
        }
        pio_sm_set_enabled(swd->pio, swd->sm, false);
        swd->active = false;
    }
    probe_release_pads(swd);
    probe_reset_set(swd, false);
    return ret;
}

int mclw_hal_swd_set_clock(struct mclw_swd *swd, uint32_t hz) {
    if (swd == NULL || swd->owner == NULL) {
        return -EINVAL;
    }
    swd->request_hz = hz < PROBE_MIN_CLOCK_HZ ? PROBE_MIN_CLOCK_HZ : hz;
    probe_apply_clock(swd);
    return 0;
}

uint32_t mclw_hal_swd_get_clock(const struct mclw_swd *swd) {
    return swd == NULL ? 0u : swd->clock_hz;
}

int mclw_hal_swd_configure(struct mclw_swd *swd, uint8_t turnaround, bool data_phase) {
    if (swd == NULL || swd->owner == NULL) {
        return -EINVAL;
    }
    if (turnaround == 0u || turnaround > 4u) {
        return -EINVAL;
    }
    swd->turnaround = turnaround;
    swd->data_phase = data_phase;
    return 0;
}

int mclw_hal_swd_output_sequence(struct mclw_swd *swd, uint32_t count,
    const uint8_t *data) {
    if (swd == NULL || swd->owner == NULL || (count != 0u && data == NULL)) {
        return -EINVAL;
    }
    if (!swd->active) {
        return -EACCES;
    }
    while (count > 0u) {
        uint32_t chunk = probe_min(count, 8u);
        int ret = probe_write(swd, chunk, *data++);

        if (ret != 0) {
            return ret;
        }
        count -= chunk;
    }
    return 0;
}

int mclw_hal_swd_input_sequence(struct mclw_swd *swd, uint32_t count, uint8_t *data) {
    if (swd == NULL || swd->owner == NULL || (count != 0u && data == NULL)) {
        return -EINVAL;
    }
    if (!swd->active) {
        return -EACCES;
    }
    while (count > 0u) {
        uint32_t chunk = probe_min(count, 8u);
        uint32_t value = 0u;
        int ret = probe_read(swd, chunk, &value);

        if (ret != 0) {
            return ret;
        }
        *data++ = (uint8_t)value;
        count -= chunk;
    }
    return 0;
}

int mclw_hal_swd_transfer(struct mclw_swd *swd, uint8_t request, uint32_t *data,
    uint8_t idle_cycles, uint8_t *response) {
    uint32_t parity = 0u;
    uint32_t value = 0u;
    uint32_t raw_ack = 0u;
    uint8_t packet = (1u << 0) | (1u << 7);
    uint8_t ack;
    int ret;

    if (swd == NULL || swd->owner == NULL || response == NULL) {
        return -EINVAL;
    }
    if (!swd->active) {
        return -EACCES;
    }
    for (uint32_t bit = 0u; bit < 4u; bit++) {
        uint32_t request_bit = ((uint32_t)request >> bit) & 1u;

        packet |= (uint8_t)(request_bit << (bit + 1u));
        parity += request_bit;
    }
    packet |= (uint8_t)((parity & 1u) << 5);

    ret = probe_write(swd, 8u, packet);
    if (ret != 0) {
        return ret;
    }
    ret = probe_read(swd, (uint32_t)swd->turnaround + 3u, &raw_ack);
    if (ret != 0) {
        return ret;
    }
    ack = (uint8_t)(raw_ack >> swd->turnaround);

    if (ack == MCLW_SWD_ACK_OK) {
        if ((request & PROBE_REQUEST_RNW) != 0u) {
            ret = probe_read(swd, 32u, &value);
            if (ret == 0) {
                ret = probe_read(swd, 1u, &parity);
            }
            if (ret != 0) {
                return ret;
            }
            if (((uint32_t)__builtin_popcount(value) ^ parity) & 1u) {
                ack |= MCLW_SWD_ERROR;
            }
            if (data != NULL) {
                *data = value;
            }
            ret = probe_hiz(swd, swd->turnaround);
        } else {
            ret = probe_hiz(swd, swd->turnaround);
            value = data != NULL ? *data : 0u;
            if (ret == 0) {
                ret = probe_write(swd, 32u, value);
            }
            if (ret == 0) {
                ret = probe_write(swd, 1u, (uint32_t)__builtin_popcount(value) & 1u);
            }
        }
        while (ret == 0 && idle_cycles > 0u) {
            uint32_t chunk = probe_min((uint32_t)idle_cycles, 32u);

            ret = probe_write(swd, chunk, 0u);
            idle_cycles -= (uint8_t)chunk;
        }
    } else if (ack == MCLW_SWD_ACK_WAIT || ack == MCLW_SWD_ACK_FAULT) {
        if (swd->data_phase && (request & PROBE_REQUEST_RNW) != 0u) {
            uint32_t discarded = 0u;

            ret = probe_read(swd, 32u, &discarded);
            if (ret == 0) {
                ret = probe_read(swd, 1u, &discarded);
            }
        }
        if (ret == 0) {
            ret = probe_hiz(swd, swd->turnaround);
        }
        if (ret == 0 && swd->data_phase && (request & PROBE_REQUEST_RNW) == 0u) {
            ret = probe_write(swd, 32u, 0u);
            if (ret == 0) {
                ret = probe_write(swd, 1u, 0u);
            }
        }
    } else {
        uint32_t backoff = (uint32_t)swd->turnaround + 33u;

        while (ret == 0 && backoff > 0u) {
            uint32_t chunk = probe_min(backoff, 32u);
            uint32_t discarded = 0u;

            ret = probe_read(swd, chunk, &discarded);
            backoff -= chunk;
        }
    }

    *response = ack;
    return ret;
}

int mclw_hal_swd_set_pins(struct mclw_swd *swd, uint8_t mask, uint8_t values) {
    uint64_t pin_mask = 0u;
    uint64_t levels = 0u;

    if (swd == NULL || swd->owner == NULL) {
        return -EINVAL;
    }
    if ((mask & MCLW_SWD_PIN_NRESET) != 0u) {
        probe_reset_set(swd, (values & MCLW_SWD_PIN_NRESET) == 0u);
    }
    if ((mask & (MCLW_SWD_PIN_SWCLK | MCLW_SWD_PIN_SWDIO)) == 0u) {
        return 0;
    }
    if (!swd->active) {
        return -EACCES;
    }
    if ((mask & MCLW_SWD_PIN_SWCLK) != 0u) {
        pin_mask |= MCLW_PIO_PIN(swd->swclk);
        if ((values & MCLW_SWD_PIN_SWCLK) != 0u) {
            levels |= MCLW_PIO_PIN(swd->swclk);
        }
    }
    if ((mask & MCLW_SWD_PIN_SWDIO) != 0u) {
        pin_mask |= MCLW_PIO_PIN(swd->swdio);
        if ((values & MCLW_SWD_PIN_SWDIO) != 0u) {
            levels |= MCLW_PIO_PIN(swd->swdio);
        }
    }
    (void)probe_wait_idle(swd);
    pio_sm_set_enabled(swd->pio, swd->sm, false);
    pio_sm_set_pins_with_mask64(swd->pio, swd->sm, levels, pin_mask);
    pio_sm_set_pindirs_with_mask64(swd->pio, swd->sm, pin_mask, pin_mask);
    probe_resync(swd);
    return 0;
}

int mclw_hal_swd_get_pins(struct mclw_swd *swd, uint8_t *values) {
    uint8_t result = 0u;

    if (swd == NULL || swd->owner == NULL || values == NULL) {
        return -EINVAL;
    }
    if (gpio_get(swd->swclk)) {
        result |= MCLW_SWD_PIN_SWCLK;
    }
    if (gpio_get(swd->swdio)) {
        result |= MCLW_SWD_PIN_SWDIO;
    }
    if (probe_reset_get(swd)) {
        result |= MCLW_SWD_PIN_NRESET;
    }
    *values = result;
    return 0;
}
