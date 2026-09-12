// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <stddef.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/i2c_slave.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"

#define I2C_TARGET_CLOCK_HZ 400000

struct metalclaw_i2c_target {
    i2c_inst_t *i2c;
    uint8_t sda;
    uint8_t scl;
    uint8_t address;
    const void *owner;
    struct metalclaw_regmap *image;
    // Set by the master's first written byte; auto-incremented after use.
    uint8_t pointer;
    bool pointer_pending;
    uint32_t transactions;
};

static struct metalclaw_i2c_target targets[2];

static i2c_inst_t *instance_for(uint8_t sda, uint8_t scl) {
    // GPIO function I2C maps even pins to SDA and odd to SCL; the block is
    // (pin / 2) % 2, the same for both pins of one bus.
    if (sda >= 30 || scl >= 30 || (sda & 1u) != 0 || (scl & 1u) != 1 || ((sda / 2) & 1u) != ((scl / 2) & 1u)) {
        return NULL;
    }
    return ((sda / 2) & 1u) ? i2c1 : i2c0;
}

static void handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    struct metalclaw_i2c_target *t = &targets[i2c_hw_index(i2c)];
    struct metalclaw_regmap *image = t->image;

    switch (event) {
        case I2C_SLAVE_RECEIVE: {
            uint8_t byte = i2c_read_byte_raw(i2c);
            if (t->pointer_pending) {
                t->pointer = byte;
                t->pointer_pending = false;
            } else if (image != NULL) {
                metalclaw_regmap_bus_write(image, t->pointer++, byte);
            }
            break;
        }
        case I2C_SLAVE_REQUEST:
            i2c_write_byte_raw(i2c, image != NULL ? metalclaw_regmap_bus_read(image, t->pointer++) : 0xff);
            break;
        case I2C_SLAVE_FINISH:
            t->pointer_pending = true;
            t->transactions++;
            break;
    }
}

struct metalclaw_i2c_target *metalclaw_hal_i2c_target_open(uint8_t sda, uint8_t scl, uint8_t address,
    const void *owner, int *err) {
    i2c_inst_t *i2c = instance_for(sda, scl);
    if (i2c == NULL || address > 0x7f || owner == NULL) {
        *err = -EINVAL;
        return NULL;
    }
    struct metalclaw_i2c_target *t = &targets[i2c_hw_index(i2c)];
    if (t->owner != NULL) {
        *err = -EBUSY;
        return NULL;
    }
    *err = metalclaw_hal_pin_claim(sda, owner);
    if (*err == 0) {
        *err = metalclaw_hal_pin_claim(scl, owner);
        if (*err != 0) {
            metalclaw_hal_pin_release(sda, owner);
        }
    }
    if (*err != 0) {
        return NULL;
    }
    t->i2c = i2c;
    t->sda = sda;
    t->scl = scl;
    t->address = address;
    t->owner = owner;
    t->image = NULL;
    t->pointer = 0;
    t->pointer_pending = true;
    t->transactions = 0;

    gpio_init(sda);
    gpio_init(scl);
    gpio_set_function(sda, GPIO_FUNC_I2C);
    gpio_set_function(scl, GPIO_FUNC_I2C);
    gpio_pull_up(sda);
    gpio_pull_up(scl);
    i2c_init(i2c, I2C_TARGET_CLOCK_HZ);
    i2c_slave_init(i2c, address, handler);
    return t;
}

void metalclaw_hal_i2c_target_close(struct metalclaw_i2c_target *t) {
    if (t == NULL || t->owner == NULL) {
        return;
    }
    i2c_slave_deinit(t->i2c);
    i2c_deinit(t->i2c);
    gpio_deinit(t->sda);
    gpio_deinit(t->scl);
    metalclaw_hal_pin_release(t->sda, t->owner);
    metalclaw_hal_pin_release(t->scl, t->owner);
    t->owner = NULL;
    t->image = NULL;
}

void metalclaw_hal_i2c_target_arm(struct metalclaw_i2c_target *t, struct metalclaw_regmap *image) {
    uint32_t state = metalclaw_hal_critical_enter();
    t->image = image;
    t->pointer_pending = true;
    metalclaw_hal_critical_exit(state);
}

void metalclaw_hal_i2c_target_disarm(struct metalclaw_i2c_target *t) {
    metalclaw_hal_i2c_target_arm(t, NULL);
}

uint32_t metalclaw_hal_i2c_target_transactions(const struct metalclaw_i2c_target *t) {
    return t->transactions;
}

uint8_t metalclaw_hal_i2c_target_address(const struct metalclaw_i2c_target *t) {
    return t->address;
}
