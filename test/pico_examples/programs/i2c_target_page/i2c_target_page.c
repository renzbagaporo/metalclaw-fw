// SPDX-License-Identifier: Apache-2.0 OR MIT
// The rig's own DUT program for i2c_page_controller: an I2C target at
// 0x42 on the board's default I2C pins (i2c0, GP4 SDA, GP5 SCL), holding a
// 256-byte page that writes fill and reads serve from one position that
// wraps at 256, with no address byte. pico-examples' slave_mem_i2c has
// the same target, but its main() ends once its own controller (wired to
// the target there) finds no target, taking the target down with it.

#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/i2c_slave.h"
#include "pico/stdlib.h"

#define TARGET_ADDRESS 0x42
#define TARGET_HZ 400000

static uint8_t page[256];
static uint8_t position;
static volatile uint32_t written;
static volatile uint32_t read;

static void handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    switch (event) {
        case I2C_SLAVE_RECEIVE:
            page[position++] = i2c_read_byte_raw(i2c);
            written++;
            break;
        case I2C_SLAVE_REQUEST:
            i2c_write_byte_raw(i2c, page[position++]);
            read++;
            break;
        case I2C_SLAVE_FINISH:
            break;
    }
}

int main(void) {
    stdio_init_all();
    gpio_init(PICO_DEFAULT_I2C_SDA_PIN);
    gpio_set_function(PICO_DEFAULT_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PICO_DEFAULT_I2C_SDA_PIN);
    gpio_init(PICO_DEFAULT_I2C_SCL_PIN);
    gpio_set_function(PICO_DEFAULT_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PICO_DEFAULT_I2C_SCL_PIN);
    i2c_init(i2c0, TARGET_HZ);
    i2c_slave_init(i2c0, TARGET_ADDRESS, &handler);
    printf("I2C target page at 0x%02x\n", TARGET_ADDRESS);
    uint32_t last_written = 0, last_read = 0;
    for (;;) {
        sleep_ms(500);
        if (written != last_written || read != last_read) {
            last_written = written;
            last_read = read;
            printf("written %lu read %lu\n", (unsigned long)last_written, (unsigned long)last_read);
        }
    }
}
