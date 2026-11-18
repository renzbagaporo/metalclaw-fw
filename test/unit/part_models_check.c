// SPDX-License-Identifier: Apache-2.0 OR MIT
// Runs a part model on the host behind a stand-in for the rp2 I2C target
// engine (core/ports/rp2/hal/i2c_target.c: the first written byte selects
// the pointer through the image's pointer map, every further byte moves it
// on, and each transaction ends with mclw_regmap_bus_restart, one per
// i2c_*_blocking call as the SDK raises I2C_SLAVE_FINISH at a STOP or a
// repeated START), with a simulated clock ticking the model's
// time_advanced once a millisecond. Against it runs a transcription of one pico-examples
// program's register traffic and arithmetic (pico-examples 0d62f75, the
// file and lines cited per program), printing what the program prints.
//
//   part_models_check <target> <model> <address> [property=v[,v...]]...
//   part_models_check behaviours
//
// The second form checks what no program's output shows, for those parts
// and the MCP9808: reset values, identity registers, the reset bits, bursts through the pointer maps, the
// INA228's accumulators, the conversion-ready flag a read clears, and where
// a transaction starts with and without a pointer map.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"

#include "ina2xx.h"
#include "lis3dh.h"
#include "mcp9808.h"
#include "mma8451q.h"
#include "mpl3115a2.h"
#include "mpu6050.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("failed: %s (line %d)\n", #cond, __LINE__); \
            exit(1); \
        } \
    } while (0)

struct mclw_i2c_target {
    struct mclw_regmap *image;
    uint8_t pointer;
    uint8_t selected;
};

struct mclw_gpio {
    bool level;
};

static struct mclw_i2c_target target;
static struct mclw_gpio int1 = { .level = false };
static struct mclw_instance instance;
static uint64_t now_us;
static uint64_t ticked_ms;
static uint32_t bus_hz = 100000;
static uint8_t part_address;

uint32_t mclw_hal_critical_enter(void) {
    return 0;
}

void mclw_hal_critical_exit(uint32_t state) {
    (void)state;
}

int mclw_hal_i2c_target_arm(struct mclw_i2c_target *t, struct mclw_regmap *image) {
    t->image = image;
    if (image != NULL) {
        t->selected = mclw_regmap_bus_select(image, 0);
        t->pointer = mclw_regmap_bus_restart(image, t->selected, t->pointer);
    }
    return 0;
}

void mclw_hal_i2c_target_disarm(struct mclw_i2c_target *t) {
    t->image = NULL;
}

void mclw_hal_gpio_write(struct mclw_gpio *gpio, bool level) {
    gpio->level = level;
}

// A model's pulse on a pin: no time passes here.
void mclw_hal_delay_us(uint32_t us) {
    (void)us;
}

void mclw_trace(struct mclw_instance *inst, enum mclw_trace_kind kind, uint32_t addr, uint16_t len,
    int16_t result) {
    (void)inst;
    (void)kind;
    (void)addr;
    (void)len;
    (void)result;
}

static void run_clock(uint64_t us) {
    now_us += us;
    while ((ticked_ms + 1) * 1000 <= now_us) {
        ticked_ms++;
        if (instance.model != NULL && instance.model->ops->time_advanced != NULL) {
            instance.model->ops->time_advanced(&instance, ticked_ms * 1000);
        }
    }
}

static void sleep_ms(uint32_t ms) {
    run_clock((uint64_t)ms * 1000);
}

// Nine clocks a byte plus the address byte, and a little for START/STOP.
static void bus_time(size_t bytes) {
    run_clock(((uint64_t)(bytes + 1) * 9 * 1000000 + bus_hz - 1) / bus_hz + 10);
}

static int i2c_write_blocking(uint8_t addr, const uint8_t *buf, size_t len, bool nostop) {
    (void)nostop;
    CHECK(addr == part_address);
    bus_time(len);
    if (len == 0) {
        return 0;
    }
    target.selected = mclw_regmap_bus_select(target.image, buf[0]);
    target.pointer = target.selected;
    for (size_t i = 1; i < len; i++) {
        mclw_regmap_bus_write(target.image, target.pointer++, buf[i]);
    }
    target.pointer = mclw_regmap_bus_restart(target.image, target.selected, target.pointer);
    return (int)len;
}

static int i2c_read_blocking(uint8_t addr, uint8_t *buf, size_t len, bool nostop) {
    (void)nostop;
    CHECK(addr == part_address);
    bus_time(len);
    for (size_t i = 0; i < len; i++) {
        buf[i] = mclw_regmap_bus_read(target.image, target.pointer++);
    }
    target.pointer = mclw_regmap_bus_restart(target.image, target.selected, target.pointer);
    return (int)len;
}

static void attach(const struct mclw_model *model) {
    struct mclw_binding bindings[MCLW_INSTANCE_BINDINGS_MAX];

    memset(bindings, 0, sizeof(bindings));
    instance.model = model;
    instance.state = calloc(1, model->state_size);
    CHECK(instance.state != NULL);
    for (size_t i = 0; i < model->binding_count; i++) {
        bindings[i].kind = model->bindings[i].kind;
        if (model->bindings[i].kind == MCLW_BINDING_I2C_TARGET) {
            bindings[i].present = true;
            bindings[i].i2c = &target;
        } else if (strcmp(model->bindings[i].name, "int1") == 0) {
            bindings[i].present = true;
            bindings[i].gpio = &int1;
        }
    }
    CHECK(model->ops->init(&instance, bindings) == 0);
    CHECK(target.image != NULL);
}

static void set_property(const char *assignment) {
    const struct mclw_model *model = instance.model;
    const char *eq = strchr(assignment, '=');
    struct mclw_value value;
    size_t index = model->property_count;

    CHECK(eq != NULL);
    for (size_t i = 0; i < model->property_count; i++) {
        if (strlen(model->properties[i].name) == (size_t)(eq - assignment)
            && strncmp(model->properties[i].name, assignment, (size_t)(eq - assignment)) == 0) {
            index = i;
        }
    }
    CHECK(index < model->property_count);
    memset(&value, 0, sizeof(value));
    for (const char *p = eq + 1; *p != '\0'; value.count++) {
        char *end;
        CHECK(value.count < MCLW_VALUE_MAX_COUNT);
        value.v[value.count] = strtoll(p, &end, 0);
        CHECK(value.v[value.count] >= model->properties[index].min);
        CHECK(value.v[value.count] <= model->properties[index].max);
        p = *end == ',' ? end + 1 : end;
    }
    CHECK(value.count == model->properties[index].count);
    CHECK(model->ops->set(&instance, index, &value) == 0);
}

// 16-bit registers as ina219_i2c.c:33-48 and ina237_i2c.c:36-52 access them.
static void write_reg16(uint8_t reg, uint16_t value) {
    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    CHECK(i2c_write_blocking(part_address, buf, 3, false) == 3);
}

static uint32_t read_be(uint8_t reg, size_t len) {
    uint8_t buf[5];
    uint32_t v = 0;

    CHECK(i2c_write_blocking(part_address, &reg, 1, true) == 1);
    CHECK(i2c_read_blocking(part_address, buf, len, false) == (int)len);
    for (size_t i = 0; i < len; i++) {
        v = (v << 8) | buf[i];
    }
    return v;
}

static uint16_t read_reg16(uint8_t reg) {
    return (uint16_t)read_be(reg, 2);
}

// i2c/ina219_i2c/ina219_i2c.c:17-31, 59-102.
static void ina219_program(void) {
    bus_hz = 100 * 1000;
    printf("INA219 example\n");
    write_reg16(0x00, 0x06EFu);
    write_reg16(0x05, 4096u);
    for (int n = 0; n < 3; n++) {
        write_reg16(0x05, 4096u);
        float vshunt_mv = (int16_t)read_reg16(0x01) * 0.01f;
        uint16_t bus_raw = read_reg16(0x02);
        float vbus_v = (bus_raw >> 3) * 4.0f / 1000.0f;
        float ma = (int16_t)read_reg16(0x04) * 0.1f;
        float mw = read_reg16(0x03) * 2.0f;
        float vin_v = vbus_v + vshunt_mv / 1000.0f;
        printf("VIN+: %.3f V  VIN-: %.3f V  shunt: %.3f mV  current: %.2f mA  power: %.2f mW\n",
            (double)vin_v, (double)vbus_v, (double)vshunt_mv, (double)ma, (double)mw);
        if (bus_raw & 0x01u) {
            printf("Math overflow - measurement out of range\n");
        }
        sleep_ms(1000);
    }
}

// i2c/ina228_i2c/ina228_i2c.c, stock build (SHUNT_BOARD_15M, ADCRANGE 0,
// AVG_SAMPLES 16, CONV_TIME_US 1052): lines 23-24, 117-154, 163-308.
#define INA228_MAX_EXPECTED_CURRENT 0.2
#define INA228_R_SHUNT 0.015
static const double INA228_VSHUNT_FACTOR = 312.5 * 1e-9;
static const double INA228_VBUS_FACTOR = 195.3125 * 1e-6;
static const double INA228_DIETEMP_FACTOR = 7.8125 * 1e-3;
static const double INA228_CURRENT_FACTOR = INA228_MAX_EXPECTED_CURRENT / (double)(1u << 19);
static const double INA228_POWER_FACTOR = 3.2 * (INA228_MAX_EXPECTED_CURRENT / (double)(1u << 19));
static const uint16_t INA228_SHUNT_CAL =
    (uint16_t)(13107.2e6 * (INA228_MAX_EXPECTED_CURRENT / (double)(1u << 19)) * INA228_R_SHUNT);

static int32_t sign_extend_20(uint32_t v) {
    return (v & 0x80000u) ? (int32_t)(v | 0xFFF00000u) : (int32_t)v;
}

static uint32_t u20(uint32_t v24) {
    return v24 >> 4;
}

static void ina228_program(void) {
    bus_hz = 400 * 1000;
    write_reg16(0x00, 0x0000);
    write_reg16(0x02, INA228_SHUNT_CAL);
    write_reg16(0x01, (0xFu << 12) | (5u << 9) | (5u << 6) | (5u << 3) | 2u);
    for (int n = 0; n < 3; n++) {
        float vshunt = (float)(sign_extend_20(u20(read_be(0x04, 3))) * INA228_VSHUNT_FACTOR);
        float vbus = (float)(u20(read_be(0x05, 3)) * INA228_VBUS_FACTOR);
        float dietemp = (float)((int16_t)read_be(0x06, 2) * INA228_DIETEMP_FACTOR);
        float current = (float)(sign_extend_20(u20(read_be(0x07, 3))) * INA228_CURRENT_FACTOR);
        float power = (float)(read_be(0x08, 3) * INA228_POWER_FACTOR);
        (void)read_be(0x09, 5);
        (void)read_be(0x0A, 5);
        (void)vshunt;
        (void)dietemp;
        float expected_power = vbus * current;
        float diff = fabsf(expected_power - power);
        float tol = 1e-3f + 0.25f * fmaxf(fabsf(expected_power), fabsf(power));
        if (!(diff <= tol)) {
            printf("current: invalid  voltage: %.3f V  power: invalid\n", (double)vbus);
        } else {
            printf("current: %.2f mA  voltage: %.3f V  power: %.2f mW\n",
                (double)current * 1e3, (double)vbus, (double)power * 1000);
        }
        sleep_ms(1000);
    }
}

// i2c/ina237_i2c/ina237_i2c.c:25-34, 84-139.
#define INA237_SHUNT_RES_OHMS 0.015f
#define INA237_MAX_CURRENT_A 0.25f
#define INA237_CURRENT_LSB_A (INA237_MAX_CURRENT_A / 32768.0f)
#define INA237_SHUNT_CAL_VALUE ((uint16_t)(819.2e6f * INA237_CURRENT_LSB_A * INA237_SHUNT_RES_OHMS * 4))

static void ina237_program(void) {
    bus_hz = 100 * 1000;
    printf("INA237 example\n");
    write_reg16(0x00, 0x0010u);
    write_reg16(0x02, INA237_SHUNT_CAL_VALUE);
    for (int n = 0; n < 3; n++) {
        write_reg16(0x01, 0x7B6Au);
        int polls = 0;
        while (!(read_reg16(0x0B) & (1 << 1))) {
            CHECK(++polls < 100000);
        }
        float v = read_reg16(0x05) * 0.003125f;
        float vshunt_mv = (int16_t)read_reg16(0x04) * 1.25f / 1000.0f;
        float ma = (int16_t)read_reg16(0x07) * INA237_CURRENT_LSB_A * 1000.0f;
        float mw = read_be(0x08, 3) * 0.2f * INA237_CURRENT_LSB_A * 1000.0f;
        float mw_calc = v * ma;
        float temp_c = ((int16_t)read_reg16(0x06) >> 4) * 0.125f;
        printf("bus: %.3f V  shunt: %.3f mV  current: %.2f mA  power: %.2f mW (calc: %.2f mW)  temp: %.2f C\n",
            (double)v, (double)vshunt_mv, (double)ma, (double)mw, (double)mw_calc, (double)temp_c);
        sleep_ms(1000);
    }
}

// i2c/ina260_i2c/ina260_i2c.c:23-53, 93-127.
static void ina260_program(void) {
    bus_hz = 100 * 1000;
    printf("ina260 example\n");
    write_reg16(0x00, (4u << 9) | (0x4u << 6) | (0x4u << 3) | 0x7u);
    for (int n = 0; n < 3; n++) {
        float ma = (int16_t)read_reg16(0x01) * 1.25f;
        float v = read_reg16(0x02) * 0.00125f;
        uint32_t mw = (uint32_t)read_reg16(0x03) * 10;
        printf("current: %.2f mA voltage: %.2f V power: %lu mW\n", (double)ma, (double)v, (unsigned long)mw);
        sleep_ms(1000);
    }
}

// i2c/mpu6050_i2c/mpu6050_i2c.c:37-83, 93-119.
static void mpu6050_program(void) {
    uint8_t buf[] = { 0x6B, 0x80 };
    uint8_t buffer[6];
    int16_t accel[3], gyro[3], temp;

    bus_hz = 400 * 1000;
    printf("Hello, MPU6050! Reading raw data from registers...\n");
    i2c_write_blocking(part_address, buf, 2, false);
    sleep_ms(100);
    buf[1] = 0x00;
    i2c_write_blocking(part_address, buf, 2, false);
    sleep_ms(10);
    for (int n = 0; n < 3; n++) {
        uint8_t val = 0x3B;
        i2c_write_blocking(part_address, &val, 1, true);
        i2c_read_blocking(part_address, buffer, 6, false);
        for (int i = 0; i < 3; i++) {
            accel[i] = (int16_t)(buffer[i * 2] << 8 | buffer[(i * 2) + 1]);
        }
        val = 0x43;
        i2c_write_blocking(part_address, &val, 1, true);
        i2c_read_blocking(part_address, buffer, 6, false);
        for (int i = 0; i < 3; i++) {
            gyro[i] = (int16_t)(buffer[i * 2] << 8 | buffer[(i * 2) + 1]);
        }
        val = 0x41;
        i2c_write_blocking(part_address, &val, 1, true);
        i2c_read_blocking(part_address, buffer, 2, false);
        temp = (int16_t)(buffer[0] << 8 | buffer[1]);
        printf("Acc. X = %d, Y = %d, Z = %d\n", accel[0], accel[1], accel[2]);
        printf("Gyro. X = %d, Y = %d, Z = %d\n", gyro[0], gyro[1], gyro[2]);
        printf("Temp. = %f\n", (temp / 340.0) + 36.53);
        sleep_ms(100);
    }
}

// i2c/lis3dh_i2c/lis3dh_i2c.c:27-126.
static float lis3dh_read_data(uint8_t reg, bool is_accel) {
    uint8_t lsb, msb;
    float scaling;
    float senstivity = 0.004f;

    i2c_write_blocking(part_address, &reg, 1, true);
    i2c_read_blocking(part_address, &lsb, 1, false);
    reg |= 0x01;
    i2c_write_blocking(part_address, &reg, 1, true);
    i2c_read_blocking(part_address, &msb, 1, false);
    uint16_t raw = (uint16_t)((msb << 8) | lsb);
    scaling = is_accel ? 64 / senstivity : 64;
    return (float)((int16_t)raw) / scaling;
}

static void lis3dh_program(void) {
    uint8_t buf[2];

    bus_hz = 400 * 1000;
    printf("Hello, LIS3DH! Reading raw data from registers...\n");
    buf[0] = 0x20;
    buf[1] = 0x97;
    i2c_write_blocking(part_address, buf, 2, false);
    buf[0] = 0x23;
    buf[1] = 0x80;
    i2c_write_blocking(part_address, buf, 2, false);
    buf[0] = 0x1F;
    buf[1] = 0xC0;
    i2c_write_blocking(part_address, buf, 2, false);
    for (int n = 0; n < 3; n++) {
        float x = lis3dh_read_data(0x28, true);
        float y = lis3dh_read_data(0x2A, true);
        float z = lis3dh_read_data(0x2C, true);
        float temp = lis3dh_read_data(0x0C, false);
        printf("TEMPERATURE: %.3f%cC\n", (double)temp, 176);
        printf("ACCELERATION VALUES: \n");
        printf("X acceleration: %.3fg\n", (double)x);
        printf("Y acceleration: %.3fg\n", (double)y);
        printf("Z acceleration: %.3fg\n", (double)z);
        sleep_ms(500);
        printf("\033[1;1H\033[2J");
    }
}

// i2c/mma8451_i2c/mma8451_i2c.c:26-125.
static float mma8451_convert_accel(uint16_t raw_accel) {
    const float count = 2048;
    float acceleration;

    if ((raw_accel & 0x2000) == 0x2000) {
        raw_accel &= 0x1FFF;
        acceleration = (-8192 + (float)raw_accel) / count;
    } else {
        acceleration = (float)raw_accel / count;
    }
    acceleration *= 9.81f;
    return acceleration;
}

static float mma8451_axis(uint8_t reg) {
    uint8_t buf[2];

    i2c_write_blocking(part_address, &reg, 1, true);
    i2c_read_blocking(part_address, buf, 2, false);
    return mma8451_convert_accel((uint16_t)(buf[0] << 6 | buf[1] >> 2));
}

static void mma8451_program(void) {
    uint8_t buf[2];

    bus_hz = 400 * 1000;
    printf("Hello, MMA8451! Reading raw data from registers...\n");
    buf[0] = 0x2A;
    buf[1] = 0x00;
    i2c_write_blocking(part_address, buf, 2, false);
    buf[0] = 0x0E;
    buf[1] = 0x01;
    i2c_write_blocking(part_address, buf, 2, false);
    buf[0] = 0x2A;
    buf[1] = 0x01;
    i2c_write_blocking(part_address, buf, 2, false);
    for (int n = 0; n < 3; n++) {
        float x = mma8451_axis(0x01);
        float y = mma8451_axis(0x03);
        float z = mma8451_axis(0x05);
        printf("ACCELERATION VALUES: \n");
        printf("X acceleration: %.6fms^-2\n", (double)x);
        printf("Y acceleration: %.6fms^-2\n", (double)y);
        printf("Z acceleration: %.6fms^-2\n", (double)z);
        sleep_ms(500);
        printf("\033[1;1H\033[2J");
    }
}

// i2c/mpl3115a2_i2c/mpl3115a2_i2c.c:28-232. INT1 is level-low: the GPIO
// callback runs again for as long as the line stays low.
static uint8_t mpl_fifo[32 * 5];
static bool mpl_has_new_data;
static int mpl_callbacks;

static void mpl3115a2_write(uint8_t reg, uint8_t value) {
    uint8_t buf[] = { reg, value };
    i2c_write_blocking(part_address, buf, 2, false);
}

static void mpl3115a2_service_int1(void) {
    for (int entries = 0; !int1.level; entries++) {
        uint8_t reg = 0x01;
        uint8_t read;
        CHECK(entries < 4);
        mpl_callbacks++;
        i2c_write_blocking(part_address, &reg, 1, true);
        i2c_read_blocking(part_address, mpl_fifo, sizeof(mpl_fifo), false);
        reg = 0x00;
        i2c_write_blocking(part_address, &reg, 1, true);
        i2c_read_blocking(part_address, &read, 1, false);
        mpl_has_new_data = true;
    }
}

static void mpl3115a2_program(void) {
    int printed = 0;

    bus_hz = 400 * 1000;
    printf("Hello, MPL3115A2. Waiting for something to interrupt me!...\n");
    mpl3115a2_write(0x26, 0xB8);
    mpl3115a2_write(0x27, 0x00);
    mpl3115a2_write(0x28, 0x01);
    mpl3115a2_write(0x29, 0x40);
    mpl3115a2_write(0x2A, 0x40);
    uint16_t bars = (uint16_t)(1013.25f * 50);
    uint8_t sealevel[] = { 0x14, (uint8_t)((bars >> 8) & 0xFF), (uint8_t)(bars & 0xFF) };
    i2c_write_blocking(part_address, sealevel, 3, false);
    mpl3115a2_write(0x0F, 0x80);
    mpl3115a2_write(0x26, 0xB9);
    for (uint64_t start = now_us; printed < 2 && now_us - start < 100000000u;) {
        mpl3115a2_service_int1();
        if (mpl_has_new_data) {
            float tsum = 0, hsum = 0;
            for (int i = 0; i < 32; i++) {
                int32_t h = (int32_t)((uint32_t)mpl_fifo[i * 5] << 24);
                h |= (int32_t)mpl_fifo[i * 5 + 1] << 16;
                h |= (int32_t)mpl_fifo[i * 5 + 2] << 8;
                int16_t t = (int16_t)(mpl_fifo[i * 5 + 3] << 8);
                t = (int16_t)(t | mpl_fifo[i * 5 + 4]);
                tsum += ((float)t) / 256.f;
                hsum += ((float)h) / 65536.f;
            }
            printf("%d sample average -> t: %.4f C, h: %.4f m\n", 32, (double)(tsum / 32), (double)(hsum / 32));
            CHECK(read_reg16(0x14) == bars);
            mpl_has_new_data = false;
            printed++;
            printf("(at %.3f s, %d callbacks)\n", (double)now_us / 1e6, mpl_callbacks);
        }
        sleep_ms(10);
    }
}

static void detach(void) {
    instance.model->ops->deinit(&instance);
    free(instance.state);
    memset(&instance, 0, sizeof(instance));
}

static void fresh(const struct mclw_model *model, uint8_t address) {
    now_us = 0;
    ticked_ms = 0;
    part_address = address;
    attach(model);
}

static uint8_t read8(uint8_t reg) {
    return (uint8_t)read_be(reg, 1);
}

static void write8(uint8_t reg, uint8_t value) {
    uint8_t buf[] = { reg, value };
    CHECK(i2c_write_blocking(part_address, buf, 2, false) == 2);
}

static uint64_t read40(uint8_t reg) {
    uint8_t buf[5];
    uint64_t v = 0;

    CHECK(i2c_write_blocking(part_address, &reg, 1, true) == 1);
    CHECK(i2c_read_blocking(part_address, buf, 5, false) == 5);
    for (size_t i = 0; i < 5; i++) {
        v = (v << 8) | buf[i];
    }
    return v;
}

// The engine's pointer over a bare map, without and with a pointer map
// (pointer p at image byte 2p + 0x40, a part with 16-bit registers).
static void restarts(void) {
    static struct mclw_regmap map;
    static uint8_t pointers[MCLW_REGMAP_BANK_SIZE];
    uint8_t buf[3];

    mclw_regmap_init(&map, 1, 0, 0x00);
    for (unsigned a = 0; a < MCLW_REGMAP_BANK_SIZE; a++) {
        mclw_regmap_set(&map, 0, (uint8_t)a, (uint8_t)(a ^ 0x5A), MCLW_REG_WRITABLE);
        pointers[a] = (uint8_t)(2 * a + 0x40);
    }
    part_address = 0x50;
    target.pointer = 0x33;
    CHECK(mclw_hal_i2c_target_arm(&target, &map) == 0);

    // No map: arming leaves the pointer, which runs on from one transaction
    // to the next.
    CHECK(i2c_read_blocking(part_address, buf, 1, false) == 1 && buf[0] == (0x33 ^ 0x5A));
    buf[0] = 0x10;
    CHECK(i2c_write_blocking(part_address, buf, 1, true) == 1);
    CHECK(i2c_read_blocking(part_address, buf, 2, false) == 2);
    CHECK(buf[0] == (0x10 ^ 0x5A) && buf[1] == (0x11 ^ 0x5A));
    CHECK(i2c_read_blocking(part_address, buf, 2, false) == 2);
    CHECK(buf[0] == (0x12 ^ 0x5A) && buf[1] == (0x13 ^ 0x5A));
    buf[0] = 0x20;
    buf[1] = 0xA1;
    buf[2] = 0xA2;
    CHECK(i2c_write_blocking(part_address, buf, 3, false) == 3);
    CHECK(i2c_read_blocking(part_address, buf, 1, false) == 1 && buf[0] == (0x22 ^ 0x5A));
    CHECK(mclw_regmap_get(&map, 0, 0x20) == 0xA1 && mclw_regmap_get(&map, 0, 0x21) == 0xA2);

    // A map: arming selects pointer 0, and every transaction starts again at
    // the register the last pointer byte selected, from its first byte.
    mclw_regmap_set_pointer_map(&map, pointers);
    CHECK(mclw_hal_i2c_target_arm(&target, &map) == 0);
    CHECK(i2c_read_blocking(part_address, buf, 2, false) == 2);
    CHECK(buf[0] == (0x40 ^ 0x5A) && buf[1] == (0x41 ^ 0x5A));
    buf[0] = 0x03;
    CHECK(i2c_write_blocking(part_address, buf, 1, true) == 1);
    for (int n = 0; n < 3; n++) {
        CHECK(i2c_read_blocking(part_address, buf, 2, false) == 2);
        CHECK(buf[0] == (0x46 ^ 0x5A) && buf[1] == (0x47 ^ 0x5A));
    }
    CHECK(i2c_read_blocking(part_address, buf, 1, false) == 1 && buf[0] == (0x46 ^ 0x5A));
    buf[0] = 0x04;
    buf[1] = 0xB1;
    buf[2] = 0xB2;
    CHECK(i2c_write_blocking(part_address, buf, 3, false) == 3);
    CHECK(i2c_read_blocking(part_address, buf, 2, false) == 2 && buf[0] == 0xB1 && buf[1] == 0xB2);
    mclw_hal_i2c_target_disarm(&target);
}

static void behaviours(void) {
    uint8_t burst[6];

    restarts();

    // INA219: reset values (SBOS448G Table 2), RST, the void FS0 bit.
    fresh(&mclw_model_ina219, 0x40);
    CHECK(read_reg16(0x00) == 0x399F);
    write_reg16(0x05, 0x1001);
    CHECK(read_reg16(0x05) == 0x1000);
    write_reg16(0x00, 0x8000);
    CHECK(read_reg16(0x00) == 0x399F && read_reg16(0x05) == 0);
    // A read with no pointer write reads the register last pointed at
    // (SBOS448G 8.5.6).
    CHECK(i2c_read_blocking(part_address, burst, 2, false) == 2 && burst[0] == 0x00 && burst[1] == 0x00);
    CHECK(read_reg16(0x00) == 0x399F);
    CHECK(i2c_read_blocking(part_address, burst, 2, false) == 2 && burst[0] == 0x39 && burst[1] == 0x9F);
    detach();

    // INA228: identity, reserved bits, the accumulators over a second of
    // continuous conversions, RSTACC.
    fresh(&mclw_model_ina228, 0x40);
    CHECK(read_reg16(0x3E) == 0x5449 && read_reg16(0x3F) == 0x2281);
    CHECK(read_reg16(0x01) == 0xFB68 && read_reg16(0x02) == 0x1000 && read_reg16(0x0B) == 0x0001);
    write_reg16(0x02, 0xFFFF);
    CHECK(read_reg16(0x02) == 0x7FFF);
    write_reg16(0x02, 75);
    set_property("bus_voltage=12000");
    set_property("current=100000");
    uint32_t power = read_be(0x08, 3);
    uint64_t before = read40(0x09);
    CHECK(power > 0);
    sleep_ms(1000);
    uint64_t energy = read40(0x09) - before;
    // Eq. 6: POWER x t / 16 per second, give or take a conversion cycle
    // (3 x 1052 us here).
    CHECK(energy * 16 >= (uint64_t)power * 99 / 100 && energy * 16 <= (uint64_t)power * 101 / 100);
    CHECK(read40(0x0A) > 0);
    write_reg16(0x00, 0x4000);
    CHECK(read40(0x09) < energy / 100 && read40(0x0A) < 1000);
    detach();

    // INA237: a triggered conversion sets CNVRF; a read of DIAG_ALRT clears
    // it and leaves MEMSTAT.
    fresh(&mclw_model_ina237, 0x40);
    CHECK(read_reg16(0x3E) == 0x5449);
    for (int n = 0; n < 2; n++) {
        write_reg16(0x01, 0x1000);
        sleep_ms(5);
        CHECK(read_reg16(0x0B) == 0x0003);
        // The read cleared the whole low byte; the model puts MEMSTAT back
        // on its next millisecond.
        sleep_ms(2);
        CHECK(read_reg16(0x0B) == 0x0001);
    }
    detach();

    // INA260: identity at FEh and FFh, CONFIG's fixed bits.
    fresh(&mclw_model_ina260, 0x40);
    CHECK(read_reg16(0xFE) == 0x5449 && read_reg16(0xFF) == 0x2270);
    write_reg16(0x00, 0x0000);
    CHECK(read_reg16(0x00) == 0x6000);
    detach();

    // MPU-6050: asleep after reset, with WHO_AM_I and zero data.
    fresh(&mclw_model_mpu6050, 0x68);
    CHECK(read8(0x75) == 0x68 && read8(0x6B) == 0x40);
    set_property("acceleration=1000,0,0");
    CHECK(read_reg16(0x3B) == 0);
    write8(0x6B, 0x00);
    CHECK(read_reg16(0x3B) == 16384);
    write8(0x6B, 0x80);
    CHECK(read8(0x6B) == 0x40 && read_reg16(0x3B) == 0);
    detach();

    // LIS3DH: WHO_AM_I, and a burst from OUT_X_L with the SUB auto-increment
    // bit set reads the six output bytes.
    fresh(&mclw_model_lis3dh, 0x18);
    CHECK(read8(0x0F) == 0x33);
    write8(0x20, 0x57);
    set_property("acceleration=100,-200,300");
    uint8_t sub = 0xA8;
    i2c_write_blocking(part_address, &sub, 1, true);
    i2c_read_blocking(part_address, burst, 6, false);
    for (uint8_t i = 0; i < 6; i++) {
        CHECK(burst[i] == read8((uint8_t)(0x28 + i)));
    }
    CHECK((int16_t)(burst[2] | burst[3] << 8) == -50 * 64);
    detach();

    // MMA8451Q: WHO_AM_I, the active-mode gate, F_READ's burst of MSBs.
    fresh(&mclw_model_mma8451q, 0x1D);
    CHECK(read8(0x0D) == 0x1A && read8(0x11) == 0x80);
    set_property("acceleration=1000,-1000,500");
    CHECK(read8(0x01) == 0);
    write8(0x2A, 0x03);
    CHECK(read8(0x0B) == 0x01);
    sub = 0x00;
    i2c_write_blocking(part_address, &sub, 1, true);
    i2c_read_blocking(part_address, burst, 4, false);
    CHECK(burst[0] == 0x0F && burst[1] == 0x40 && burst[2] == 0xC0 && burst[3] == 0x20);
    write8(0x2B, 0x40);
    CHECK(read8(0x2A) == 0x00);
    detach();

    // MCP9808: arming selects pointer 0 (RFU); a read with no pointer write
    // returns the register last selected from its MSB (DS25095A 4.1.1);
    // pointers past Resolution read zero.
    fresh(&mclw_model_mcp9808, 0x18);
    CHECK(i2c_read_blocking(part_address, burst, 2, false) == 2 && burst[0] == 0x00 && burst[1] == 0x1F);
    CHECK(read_reg16(0x06) == 0x0054 && read_reg16(0x07) == 0x0400 && read8(0x08) == 0x03);
    set_property("temperature=25250");
    uint16_t ta = read_reg16(0x05);
    CHECK((ta & 0x1FFF) == 0x0194);
    for (int n = 0; n < 2; n++) {
        CHECK(i2c_read_blocking(part_address, burst, 2, false) == 2);
        CHECK(burst[0] == (uint8_t)(ta >> 8) && burst[1] == (uint8_t)ta);
    }
    write_reg16(0x02, 0x01E0);
    CHECK(i2c_read_blocking(part_address, burst, 2, false) == 2 && burst[0] == 0x01 && burst[1] == 0xE0);
    CHECK(read_reg16(0x09) == 0 && read_reg16(0x80) == 0 && read_reg16(0xFF) == 0);
    detach();

    // MPL3115A2: WHO_AM_I, BAR_IN's reset, INT1 idle high, one-shot data.
    fresh(&mclw_model_mpl3115a2, 0x60);
    CHECK(read8(0x0C) == 0xC4 && read_reg16(0x14) == 0xC5E7);
    CHECK(int1.level);
    set_property("temperature=-12500");
    write8(0x26, 0x02);
    sleep_ms(2);
    CHECK(read8(0x26) == 0x00 && read8(0x04) == (uint8_t)(-200 >> 4) && read8(0x05) == ((-200 & 0xF) << 4));
    CHECK(read8(0x06) == 0x0E);
    detach();

    printf("ok\n");
}

static const struct {
    const char *target;
    const struct mclw_model *model;
    uint8_t address;
    void (*program)(void);
} programs[] = {
    { "ina219_i2c", &mclw_model_ina219, 0x40, ina219_program },
    { "ina228_i2c", &mclw_model_ina228, 0x40, ina228_program },
    { "ina237_i2c", &mclw_model_ina237, 0x40, ina237_program },
    { "ina260_i2c", &mclw_model_ina260, 0x40, ina260_program },
    { "mpu6050_i2c", &mclw_model_mpu6050, 0x68, mpu6050_program },
    { "lis3dh_i2c", &mclw_model_lis3dh, 0x18, lis3dh_program },
    { "mma8451_i2c", &mclw_model_mma8451q, 0x1D, mma8451_program },
    { "mpl3115a2_i2c", &mclw_model_mpl3115a2, 0x60, mpl3115a2_program },
};

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "behaviours") == 0) {
        behaviours();
        return 0;
    }
    CHECK(argc >= 4);
    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) {
        if (strcmp(programs[i].target, argv[1]) != 0) {
            continue;
        }
        CHECK(strcmp(programs[i].model->id, argv[2]) == 0);
        part_address = (uint8_t)strtol(argv[3], NULL, 0);
        CHECK(part_address == programs[i].address);
        attach(programs[i].model);
        for (int a = 4; a < argc; a++) {
            set_property(argv[a]);
        }
        programs[i].program();
        detach();
        return 0;
    }
    printf("failed: no program for %s\n", argv[1]);
    return 1;
}
