// SPDX-License-Identifier: Apache-2.0 OR MIT
// Runs the neo_m8n, pa1010d, lcd_backpack, mpu9250 and w25q16jv models on
// the host behind stand-ins for the rp2 engines, against transcriptions of
// the pico-examples programs that use them (pico-examples 0d62f75, the file
// and lines cited per program), printing what each program prints and then
// the part's state as `part.<name> = <python literal>` lines.
//
//   program_parts_check <target> <model> [property=v[,v...]]...
//   program_parts_check behaviours
//
// The stand-ins keep what the engines do that the programs can see:
//   UART      bytes leave at the line rate of 10 bits a byte; the model's
//             receive callback gets what the program sent.
//   I2C       core/ports/rp2/hal/i2c_target.c: a written byte goes to the
//             map's on_input when it has one, else selects the pointer;
//             reads run through mclw_regmap_bus_read; each call is one
//             transaction and counts its bytes.
//   SPI map   the framing's read bit and address mask, auto-increment,
//             writes committed at chip-select release.
//   command   core/ports/rp2/hal/spi_target.c's command image as the host
//             sees it: status on every byte that carries no data, the
//             memory read from the published pages and filler, the busy
//             bits set at the release of a busy opcode's window until the
//             model has taken it, and the model told of each window only after
//             the next one has been clocked, as a worker that lags a
//             window behind a host polling back to back.
// A simulated clock ticks every model's time_advanced and poll once a
// millisecond.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metalclaw/command.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/hal/uart.h"
#include "metalclaw/model.h"
#include "metalclaw/regmap.h"
#include "metalclaw/runtime.h"

#include "lcd_backpack.h"
#include "mpu9250.h"
#include "neo_m8n.h"
#include "pa1010d.h"
#include "w25q.h"

extern const struct mclw_model mclw_model_neo_m8n;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("failed: %s (line %d)\n", #cond, __LINE__); \
            exit(1); \
        } \
    } while (0)

static struct mclw_instance instance;
static uint64_t now_us;
static uint64_t ticked_ms;

uint32_t mclw_hal_critical_enter(void) {
    return 0;
}

void mclw_hal_critical_exit(uint32_t state) {
    (void)state;
}

void mclw_trace(struct mclw_instance *inst, enum mclw_trace_kind kind, uint32_t addr, uint16_t len,
    int16_t result) {
    (void)inst;
    (void)kind;
    (void)addr;
    (void)len;
    (void)result;
}

struct mclw_instance *mclw_instance_from_state(const void *state) {
    return state == instance.state ? &instance : NULL;
}

uint64_t mclw_now_us(void) {
    return now_us;
}

int mclw_spi_mode_get(struct mclw_instance *inst, size_t binding, struct mclw_value *out) {
    (void)inst;
    (void)binding;
    out->v[0] = 0;
    return 0;
}

int mclw_spi_mode_set(struct mclw_instance *inst, size_t binding, const struct mclw_value *in) {
    (void)inst;
    (void)binding;
    return in->v[0] == 0 || in->v[0] == 3 ? 0 : -1;
}

// ---- UART ----

struct mclw_uart {
    mclw_uart_rx_t rx;
    void *ctx;
    uint32_t baud;
    uint8_t queue[8192];
    size_t head;
    size_t tail;
    uint32_t sent;
    uint32_t received;
    uint64_t next_byte_us;
};

static struct mclw_uart uart = { .baud = 9600 };

void mclw_hal_uart_set_rx(struct mclw_uart *u, mclw_uart_rx_t fn, void *ctx) {
    u->rx = fn;
    u->ctx = ctx;
}

int mclw_hal_uart_set_baud(struct mclw_uart *u, uint32_t baud) {
    u->baud = baud;
    return 0;
}

uint32_t mclw_hal_uart_baud(const struct mclw_uart *u) {
    return u->baud;
}

size_t mclw_hal_uart_send(struct mclw_uart *u, const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        CHECK(u->tail - u->head < sizeof(u->queue));
        u->queue[u->tail++ % sizeof(u->queue)] = bytes[i];
    }
    return len;
}

void mclw_hal_uart_stats(const struct mclw_uart *u, struct mclw_uart_stats *out) {
    memset(out, 0, sizeof(*out));
    out->bytes_tx = u->sent;
    out->bytes_rx = u->received;
}

// The part's side of the wire: the next byte it sent, once its 10 bits
// have gone by, or -1.
static int uart_line_byte(void) {
    if (uart.head == uart.tail || now_us < uart.next_byte_us) {
        return -1;
    }
    uart.next_byte_us = now_us + 10000000u / uart.baud;
    uart.sent++;
    return uart.queue[uart.head++ % sizeof(uart.queue)];
}

static void run_clock(uint64_t us);

// The program's uart_write_blocking: the bytes reach the part at line rate.
static void uart_write(const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        run_clock(10000000u / uart.baud);
        uart.received++;
        if (uart.rx != NULL) {
            uart.rx(&uart, &bytes[i], 1, uart.ctx);
        }
    }
}

// ---- I2C ----

struct mclw_i2c_target {
    struct mclw_regmap *image;
    uint8_t pointer;
    uint8_t selected;
    struct mclw_i2c_target_stats stats;
};

static struct mclw_i2c_target i2c;
static uint32_t i2c_hz = 400000;

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

void mclw_hal_i2c_target_stats(const struct mclw_i2c_target *t, struct mclw_i2c_target_stats *out) {
    *out = t->stats;
}

static void i2c_byte_time(void) {
    run_clock((9u * 1000000u + i2c_hz - 1u) / i2c_hz);
}

static void i2c_write(const uint8_t *buf, size_t len) {
    bool pointer = true;

    run_clock(30);
    for (size_t i = 0; i < len; i++) {
        i2c_byte_time();
        if (mclw_regmap_bus_input(i2c.image, buf[i])) {
            pointer = false;
        } else if (pointer) {
            i2c.selected = mclw_regmap_bus_select(i2c.image, buf[i]);
            i2c.pointer = i2c.selected;
            pointer = false;
        } else {
            mclw_regmap_bus_write(i2c.image, i2c.pointer++, buf[i]);
        }
        i2c.stats.bytes++;
    }
    i2c.pointer = mclw_regmap_bus_restart(i2c.image, i2c.selected, i2c.pointer);
    i2c.stats.transactions++;
}

static void i2c_read(uint8_t *buf, size_t len) {
    run_clock(30);
    for (size_t i = 0; i < len; i++) {
        i2c_byte_time();
        buf[i] = mclw_regmap_bus_read(i2c.image, i2c.pointer++);
        i2c.stats.bytes++;
    }
    i2c.pointer = mclw_regmap_bus_restart(i2c.image, i2c.selected, i2c.pointer);
    i2c.stats.transactions++;
}

// ---- SPI ----

struct mclw_spi_target {
    struct mclw_regmap *map;
    struct mclw_regmap_framing framing;
    struct mclw_command_image *command;
    // The command engine's copy of what the model published.
    uint8_t status;
    bool status_forced;
    int busy_windows;
    uint8_t pages[MCLW_COMMAND_PAGES_MAX][MCLW_COMMAND_PAGE_SIZE];
    // Windows clocked but not yet handed to the model.
    uint8_t pending[2][MCLW_COMMAND_WINDOW_MAX + 1];
    size_t pending_len[2];
    bool pending_forced[2];
    size_t pending_count;
    uint32_t windows;
};

static struct mclw_spi_target spi;
static uint32_t spi_hz = 1000000;

int mclw_hal_spi_target_arm_regmap(struct mclw_spi_target *t, struct mclw_regmap *map,
    const struct mclw_regmap_framing *framing) {
    t->map = map;
    t->framing = *framing;
    return 0;
}

static void command_notify(struct mclw_command_image *image, void *ctx) {
    struct mclw_spi_target *t = ctx;

    if ((image->dirty & MCLW_COMMAND_DIRTY_STATUS) != 0 || t->status_forced) {
        t->status = (uint8_t)(image->status | (t->busy_windows != 0 ? image->busy_status : 0));
        t->status_forced = false;
    }
    if ((image->dirty & MCLW_COMMAND_DIRTY_PAGES) != 0) {
        memcpy(t->pages, image->pages, sizeof(t->pages));
    }
    image->dirty = 0;
}

int mclw_hal_spi_target_arm_command(struct mclw_spi_target *t, struct mclw_command_image *image,
    struct mclw_gpio *busy) {
    CHECK(busy == NULL);
    t->command = image;
    image->notify = command_notify;
    image->notify_ctx = t;
    image->dirty = 0xff;
    command_notify(image, t);
    t->pending_count = 0;
    return 0;
}

void mclw_hal_spi_target_disarm(struct mclw_spi_target *t) {
    t->map = NULL;
    t->command = NULL;
}

void mclw_hal_spi_target_set_busy(struct mclw_spi_target *t, bool busy) {
    (void)t;
    (void)busy;
}

static void spi_bytes_time(size_t n) {
    run_clock((uint64_t)n * 8u * 1000000u / spi_hz + 2u);
}

static void spi_regmap_window(const uint8_t *mosi, uint8_t *miso, size_t len) {
    struct mclw_regmap *map = spi.map;
    uint8_t command = mosi[0];
    uint8_t address = command & spi.framing.address_mask;

    spi_bytes_time(len);
    miso[0] = 0;
    if ((command & spi.framing.read_bit_mask) != 0) {
        for (size_t i = 1; i < len; i++) {
            miso[i] = mclw_regmap_bus_read(map, address);
            address = (uint8_t)((address + 1u) & spi.framing.address_mask);
        }
    } else {
        for (size_t i = 1; i < len; i++) {
            miso[i] = 0;
            mclw_regmap_bus_write(map, address, mosi[i]);
            address = (uint8_t)((address + 1u) & spi.framing.address_mask);
        }
    }
}

static uint8_t command_page_byte(uint16_t address) {
    const struct mclw_command_image *image = spi.command;

    for (uint8_t i = 0; i < image->page_count; i++) {
        if (image->page_number[i] == (address >> 8)) {
            return spi.pages[i][address & 0xff];
        }
    }
    return image->filler;
}

static void command_deliver(void) {
    struct mclw_command_image *image = spi.command;

    image->on_window(image, spi.pending[0], spi.pending_len[0], false, image->on_window_ctx);
    if (spi.pending_forced[0]) {
        spi.busy_windows--;
        spi.status_forced = true;
        command_notify(image, &spi);
    }
    memmove(spi.pending[0], spi.pending[1], sizeof(spi.pending[0]));
    spi.pending_len[0] = spi.pending_len[1];
    spi.pending_forced[0] = spi.pending_forced[1];
    spi.pending_count--;
}

static void spi_command_window(const uint8_t *mosi, uint8_t *miso, size_t len) {
    const struct mclw_command_image *image = spi.command;
    uint8_t opcode = mosi[0];
    bool memory = image->has_memory_read && opcode == image->read_memory_opcode;

    spi_bytes_time(len);
    for (size_t i = 0; i < len; i++) {
        miso[i] = spi.status;
    }
    if (memory && len > 4) {
        uint16_t address = (uint16_t)((mosi[2] << 8) | mosi[3]);
        for (size_t i = 4; i < len && i < 4 + 256; i++) {
            miso[i] = command_page_byte((uint16_t)(address + (i - 4)));
        }
    }
    bool forced = (image->busy_opcodes[opcode / 8] & (1u << (opcode % 8))) != 0;
    if (forced) {
        spi.status |= image->busy_status;
        spi.busy_windows++;
    }
    size_t kept = memory ? (len < 4 ? len : 4) : (len > MCLW_COMMAND_WINDOW_MAX + 1 ? MCLW_COMMAND_WINDOW_MAX + 1 : len);
    CHECK(spi.pending_count < 2);
    memcpy(spi.pending[spi.pending_count], mosi, kept);
    spi.pending_len[spi.pending_count] = kept;
    spi.pending_forced[spi.pending_count] = forced;
    spi.pending_count++;
    spi.windows++;
    while (spi.pending_count > 1) {
        command_deliver();
    }
}

// One chip-select window.
static void spi_window(const uint8_t *mosi, uint8_t *miso, size_t len) {
    if (spi.command != NULL) {
        spi_command_window(mosi, miso, len);
    } else {
        spi_regmap_window(mosi, miso, len);
    }
}

// The worker catching up while the program is idle.
static void spi_settle(void) {
    while (spi.command != NULL && spi.pending_count > 0) {
        command_deliver();
    }
}

// ---- clock and instance ----

static void run_clock(uint64_t us) {
    now_us += us;
    while ((ticked_ms + 1) * 1000 <= now_us) {
        ticked_ms++;
        if (instance.model == NULL) {
            continue;
        }
        if (instance.model->ops->time_advanced != NULL) {
            instance.model->ops->time_advanced(&instance, ticked_ms * 1000);
        }
        if (instance.model->ops->poll != NULL) {
            instance.model->ops->poll(&instance);
        }
    }
}

static void sleep_ms(uint32_t ms) {
    run_clock((uint64_t)ms * 1000);
}

static void attach(const struct mclw_model *model) {
    struct mclw_binding bindings[MCLW_INSTANCE_BINDINGS_MAX];
    bool bound = false;

    memset(bindings, 0, sizeof(bindings));
    instance.model = model;
    instance.state = calloc(1, model->state_size);
    CHECK(instance.state != NULL);
    for (size_t i = 0; i < model->binding_count; i++) {
        bindings[i].kind = model->bindings[i].kind;
        if (bound) {
            continue;
        }
        bindings[i].present = true;
        bound = true;
        if (model->bindings[i].kind == MCLW_BINDING_UART) {
            bindings[i].uart = &uart;
            uart.baud = model->bindings[i].uart.default_baud;
        } else if (model->bindings[i].kind == MCLW_BINDING_I2C_TARGET && model != &mclw_model_mpu9250) {
            bindings[i].i2c = &i2c;
        } else if (model->bindings[i].kind == MCLW_BINDING_SPI_TARGET) {
            bindings[i].spi = &spi;
        } else {
            bindings[i].present = false;
            bound = false;
        }
    }
    CHECK(model->ops->init(&instance, bindings) == 0);
}

static size_t property_index(const char *name, size_t len) {
    const struct mclw_model *model = instance.model;

    for (size_t i = 0; i < model->property_count; i++) {
        if (strlen(model->properties[i].name) == len && strncmp(model->properties[i].name, name, len) == 0) {
            return i;
        }
    }
    printf("no property %.*s\n", (int)len, name);
    exit(1);
}

static void set_property(const char *assignment) {
    const struct mclw_model *model = instance.model;
    const char *eq = strchr(assignment, '=');
    struct mclw_value value;
    size_t index;

    CHECK(eq != NULL);
    index = property_index(assignment, (size_t)(eq - assignment));
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

static int64_t get1(const char *name) {
    struct mclw_value v;

    memset(&v, 0, sizeof(v));
    CHECK(instance.model->ops->get(&instance, property_index(name, strlen(name)), &v) == 0);
    return v.v[0];
}

// Every property, and every region, as the harness REPL would give them.
static void print_part(void) {
    const struct mclw_model *model = instance.model;

    for (size_t i = 0; i < model->property_count; i++) {
        const struct mclw_property_desc *d = &model->properties[i];
        struct mclw_value v;

        memset(&v, 0, sizeof(v));
        if (model->ops->get(&instance, i, &v) != 0) {
            continue;
        }
        printf("part.%s = ", d->name);
        if (d->count > 1) {
            printf("(");
        }
        for (uint8_t k = 0; k < d->count; k++) {
            if (d->type == MCLW_TYPE_BOOL) {
                printf("%s", v.v[k] ? "True" : "False");
            } else {
                printf("%lld", (long long)v.v[k]);
            }
            printf("%s", d->count > 1 ? ", " : "");
        }
        printf("%s\n", d->count > 1 ? ")" : "");
    }
    for (size_t i = 0; i < model->region_count; i++) {
        uint8_t buf[2048];
        CHECK(model->regions[i].size <= sizeof(buf));
        CHECK(model->ops->read_region(&instance, i, 0, buf, model->regions[i].size) == 0);
        printf("region.%s = ", model->regions[i].name);
        for (uint32_t k = 0; k < model->regions[i].size; k++) {
            printf("%02x", buf[k]);
        }
        printf("\n");
    }
}

// ---- programs ----

// uart/gps_uart/gps_uart.c:70-125, 138-186, 197-257.
typedef struct {
    float latitude;
    float longitude;
    bool is_valid;
} gps_data_t;

static bool validate_nmea_checksum(char *nmea_string) {
    int len = (int)strlen(nmea_string);
    printf("Full NMEA String: %s", nmea_string);
    if (len < 7) {
        printf("Invalid: Too short (< 7 chars)\n");
        return false;
    }
    char *checksum_ptr = strchr(nmea_string, '*');
    if (!checksum_ptr) {
        printf("Invalid: No checksum marker (*) found\n");
        return false;
    }
    uint8_t calculated_checksum = 0;
    for (char *p = nmea_string + 1; p < checksum_ptr; p++) {
        calculated_checksum ^= (uint8_t)*p;
    }
    char hex_checksum[3];
    snprintf(hex_checksum, sizeof(hex_checksum), "%02X", calculated_checksum);
    bool is_valid = strncmp(hex_checksum, checksum_ptr + 1, 2) == 0;
    if (!is_valid) {
        printf("Checksum Validation: INVALID\n");
    }
    return is_valid;
}

static void convert_nmea_to_decimal(char *nmea_coord, float *decimal_coord) {
    float degrees = (float)(atof(nmea_coord) / 100.0);
    int int_degrees = (int)degrees;
    float minutes = (float)((double)(degrees - (float)int_degrees) * 100.0);
    *decimal_coord = (float)(int_degrees + ((double)minutes / 60.0));
}

static bool parse_nmea_gps(char *nmea_string, gps_data_t *gps_data) {
    if (!validate_nmea_checksum(nmea_string)) {
        printf("Invalid NMEA Checksum\n");
        return false;
    }
    if (strncmp(nmea_string, "$GPRMC", 6) != 0) {
        return false;
    }
    char *tokens[12] = { 0 };
    int token_count = 0;
    char *token = strtok(nmea_string, ",");
    while (token != NULL && token_count < 12) {
        tokens[token_count++] = token;
        token = strtok(NULL, ",");
    }
    if (token_count >= 7 && strcmp(tokens[2], "A") == 0) {
        if (strlen(tokens[3]) > 0) {
            convert_nmea_to_decimal(tokens[3], &gps_data->latitude);
            if (tokens[4][0] == 'S') {
                gps_data->latitude = -gps_data->latitude;
            }
        }
        if (strlen(tokens[5]) > 0) {
            convert_nmea_to_decimal(tokens[5], &gps_data->longitude);
            if (tokens[6][0] == 'W') {
                gps_data->longitude = -gps_data->longitude;
            }
        }
        gps_data->is_valid = true;
        return true;
    }
    return false;
}

static char uart_getc(void) {
    int c;
    while ((c = uart_line_byte()) < 0) {
        run_clock(50);
    }
    return (char)c;
}

static void gps_uart_program(void) {
    gps_data_t gps_data = { 0 };

    printf("Waiting for NMEA data\n");
    while (now_us < 4500000u) {
        char nmea_buffer[256];
        int chars_read = 0;
        while (true) {
            if (chars_read >= 256 - 1) {
                break;
            }
            char received_char = uart_getc();
            if (received_char == 0) {
                continue;
            }
            nmea_buffer[chars_read] = received_char;
            if ((int)received_char == 10) {
                nmea_buffer[chars_read + 1] = '\0';
                if (parse_nmea_gps(nmea_buffer, &gps_data)) {
                    printf("Valid GPS Data Received\n");
                }
                break;
            }
            chars_read++;
        }
        if (gps_data.is_valid) {
            printf("Latitude: %.6f\n", (double)gps_data.latitude);
            printf("Longitude: %.6f\n", (double)gps_data.longitude);
            gps_data.is_valid = false;
        }
    }
}

// i2c/pa1010d_i2c/pa1010d_i2c.c:31-111, 115-154. Where the program's
// strstr finds no GNRMC it goes on from address 0; this says so instead.
static void pa1010d_parse_string(char output[], const char protocol[]) {
    char *com_index = strstr(output, protocol);
    if (com_index == NULL) {
        printf("(no %s in the packet)\n", protocol);
        return;
    }
    int p = (int)(com_index - output);
    int n = 0;
    int m = 0;
    char gps_data[14][15];
    memset(gps_data, 0, sizeof(gps_data));
    bool complete = false;
    while (output[p] != '$' && n < 15 && complete == false) {
        if (output[p] == ',' || output[p] == '*') {
            n += 1;
            m = 0;
        } else {
            CHECK(n < 14);
            gps_data[n][m] = output[p];
            if (m < 14) {
                m++;
            } else {
                complete = true;
            }
        }
        p++;
    }
    printf("Protocol:%s\n", gps_data[0]);
    printf("UTC Time: %s\n", gps_data[1]);
    printf("Status: %s\n", gps_data[2][0] == 'V' ? "Data invalid. GPS fix not found." : "Data Valid");
    printf("Latitude: %s\n", gps_data[3]);
    printf("N/S indicator: %s\n", gps_data[4]);
    printf("Longitude: %s\n", gps_data[5]);
    printf("E/W indicator: %s\n", gps_data[6]);
    printf("Speed over ground: %s\n", gps_data[7]);
    printf("Course over ground: %s\n", gps_data[8]);
    printf("Date: %c%c/%c%c/%c%c\n", gps_data[9][0], gps_data[9][1], gps_data[9][2], gps_data[9][3], gps_data[9][4],
        gps_data[9][5]);
    printf("Magnetic Variation: %s\n", gps_data[10]);
    printf("E/W degree indicator: %s\n", gps_data[11]);
    printf("Mode: %s\n", gps_data[12]);
    printf("Checksum: %c%c\n", gps_data[13][0], gps_data[13][1]);
}

static void pa1010d_read_raw(char numcommand[]) {
    uint8_t buffer[250 + 1];
    int i = 0;
    bool complete = false;

    i2c_read(buffer, 250);
    buffer[250] = 0;
    while (i < 250 && complete == false) {
        numcommand[i] = (char)buffer[i];
        if (buffer[i] == 10 && buffer[i + 1] == 10) {
            complete = true;
        }
        i++;
    }
}

static void pa1010d_program(void) {
    char numcommand[250 + 1];
    char init_command[] = "$PMTK314,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*29\r\n";

    printf("Hello, PA1010D! Reading raw data from module...\n");
    for (size_t i = 0; i < sizeof(init_command); i++) {
        uint8_t b = (uint8_t)init_command[i];
        i2c_write(&b, 1);
    }
    for (int n = 0; n < 4; n++) {
        memset(numcommand, 0, sizeof(numcommand));
        pa1010d_read_raw(numcommand);
        pa1010d_parse_string(numcommand, "GNRMC");
        sleep_ms(1000);
        // About 60 characters of output at 115200 baud between reads.
        sleep_ms(50);
        printf("\033[1;1H\033[2J");
    }
}

// uart/lcd_uart/lcd_uart.c:62-149 as the pinned build compiles it. lcd_init
// (125) starts with Display On, whose one argument byte lcd_set_backlight
// (101) passes as a NULL buffer. GCC sees the read through NULL and, as
// -fisolate-erroneous-paths-dereference does at -O2 (CMAKE_BUILD_TYPE
// Release), puts a UDF in its place after the 0xFE and 0x42 are written:
// both builds' lcd_init end `ldrb r3, [r3, #0]; udf #255`. The program
// takes a HardFault there and sends nothing more.
static void lcd_uart_program(void) {
    static const uint8_t display_on[] = { 0xfe, 0x42 };

    uart_write(display_on, sizeof(display_on));
    sleep_ms(100);
    printf("stats.uart.bytes_rx = %u\n", (unsigned)uart.received);
    printf("stats.uart.frame_errors = 0\n");
}

// spi/mpu9250_spi/mpu9250_spi.c:63-109, 111-152.
static void mpu9250_read_registers(uint8_t reg, uint8_t *buf, uint16_t len) {
    uint8_t mosi[16] = { (uint8_t)(reg | 0x80) };
    uint8_t miso[16];

    CHECK(len < sizeof(mosi));
    sleep_ms(10);
    spi_window(mosi, miso, (size_t)len + 1);
    memcpy(buf, &miso[1], len);
    sleep_ms(10);
}

static void mpu9250_program(void) {
    uint8_t reset[] = { 0x6b, 0x00 };
    uint8_t miso[2];
    uint8_t id;
    uint8_t buffer[6];

    spi_hz = 500000;
    printf("Hello, MPU9250! Reading raw data from registers via SPI...\n");
    spi_window(reset, miso, 2);
    mpu9250_read_registers(0x75, &id, 1);
    printf("I2C address is 0x%x\n", id);
    for (int n = 0; n < 3; n++) {
        int16_t acceleration[3], gyro[3], temp;
        mpu9250_read_registers(0x3b, buffer, 6);
        for (int i = 0; i < 3; i++) {
            acceleration[i] = (int16_t)(buffer[i * 2] << 8 | buffer[(i * 2) + 1]);
        }
        mpu9250_read_registers(0x43, buffer, 6);
        for (int i = 0; i < 3; i++) {
            gyro[i] = (int16_t)(buffer[i * 2] << 8 | buffer[(i * 2) + 1]);
        }
        mpu9250_read_registers(0x41, buffer, 2);
        temp = (int16_t)(buffer[0] << 8 | buffer[1]);
        printf("Acc. X = %d, Y = %d, Z = %d\n", acceleration[0], acceleration[1], acceleration[2]);
        printf("Gyro. X = %d, Y = %d, Z = %d\n", gyro[0], gyro[1], gyro[2]);
        printf("Temp. = %f\n", (temp / 340.0) + 36.53);
        sleep_ms(100);
    }
}

// spi/spi_flash/spi_flash.c:37-157 and pio/spi/spi_flash.c:30-158: the same
// flash traffic; only the banner and the row format's code differ.
static void flash_read(uint32_t addr, uint8_t *buf, size_t len) {
    uint8_t mosi[4 + 256] = { 0x03, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    uint8_t miso[4 + 256];

    spi_window(mosi, miso, 4 + len);
    memcpy(buf, &miso[4], len);
}

static void flash_write_enable(void) {
    uint8_t mosi = 0x06;
    uint8_t miso;
    spi_window(&mosi, &miso, 1);
}

static void flash_wait_done(void) {
    uint8_t status;
    int polls = 0;
    do {
        uint8_t mosi[2] = { 0x05, 0 };
        uint8_t miso[2];
        spi_window(mosi, miso, 2);
        status = miso[1];
        CHECK(++polls < 1000000);
    } while (status & 0x01);
}

static void flash_sector_erase(uint32_t addr) {
    uint8_t mosi[4] = { 0x20, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    uint8_t miso[4];
    flash_write_enable();
    spi_window(mosi, miso, 4);
    flash_wait_done();
}

static void flash_page_program(uint32_t addr, const uint8_t data[]) {
    uint8_t mosi[4 + 256] = { 0x02, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr };
    uint8_t miso[4 + 256];
    memcpy(&mosi[4], data, 256);
    flash_write_enable();
    spi_window(mosi, miso, sizeof(mosi));
    flash_wait_done();
}

static void printbuf(const uint8_t buf[256]) {
    for (int i = 0; i < 256; ++i) {
        printf("%02x%c", buf[i], i % 16 == 15 ? '\n' : ' ');
    }
}

static void flash_program(bool pio) {
    uint8_t page_buf[256];

    if (pio) {
        // 31.25 x 4 clk_sys cycles a bit: 1 MHz at 125 MHz, 1.2 MHz at 150.
        spi_hz = 1200000;
        printf("PIO SPI Example\n");
        printf("Loaded program at %d\n", 0);
    } else {
        spi_hz = 1000000;
        printf("SPI flash example\n");
        printf("SPI initialised, let's goooooo\n");
    }
    flash_sector_erase(0);
    flash_read(0, page_buf, 256);
    printf("After erase:\n");
    printbuf(page_buf);
    for (int i = 0; i < 256; ++i) {
        page_buf[i] = (uint8_t)i;
    }
    flash_page_program(0, page_buf);
    flash_read(0, page_buf, 256);
    printf("After program:\n");
    printbuf(page_buf);
    flash_sector_erase(0);
    flash_read(0, page_buf, 256);
    printf("Erase again:\n");
    printbuf(page_buf);
    spi_settle();
    sleep_ms(100);
}

// ---- behaviours no program output shows ----

static size_t uart_take(char *out, size_t max) {
    size_t n = 0;
    while (uart.head != uart.tail && n + 1 < max) {
        out[n++] = (char)uart.queue[uart.head++ % sizeof(uart.queue)];
    }
    out[n] = '\0';
    return n;
}

static void behaviours_neo_m8n(void) {
    char text[4096];

    attach(&mclw_model_neo_m8n);
    set_property("gnss=1");
    set_property("fix=3");
    set_property("satellites=6");
    sleep_ms(1500);
    uart_take(text, sizeof(text));
    CHECK(strstr(text, "$GPRMC,") != NULL && strstr(text, "$GPGGA,") != NULL);
    CHECK(strstr(text, "$GN") == NULL && strstr(text, "$GLGSV") == NULL);
    CHECK(get1("gnss") == 1);
    CHECK(instance.model->ops->reset(&instance, false) == 0);
    CHECK(get1("gnss") == 3);
}

// The bytes of `count` packets with every <LF> dropped, as GlobalTop's guide
// (3.3) extracts NMEA from them.
static void pa1010d_collect(char *text, size_t max, int count) {
    uint8_t buf[255];
    size_t n = 0;

    for (int k = 0; k < count; k++) {
        i2c_read(buf, sizeof(buf));
        for (size_t i = 0; i < sizeof(buf) && n + 1 < max; i++) {
            if (buf[i] != 0x0a) {
                text[n++] = (char)buf[i];
            }
        }
        sleep_ms(2);
    }
    text[n] = '\0';
}

static void behaviours_pa1010d(void) {
    uint8_t buf[256];
    static char text[4096];
    const char *query = "$PMTK414*33\r\n";
    const char *gps = "$PMTK353,1,0,0,0,0*2A\r\n";

    attach(&mclw_model_pa1010d);
    set_property("fix=3");
    set_property("satellites=16");
    // No fix yet: one packet holds the start of the default epoch, the end
    // character and garbage.
    sleep_ms(1200);
    i2c_read(buf, 255);
    CHECK(memcmp(buf, "$GNGGA,", 7) == 0);
    CHECK(buf[253] != 0x0a && buf[254] == 0x0a);
    // Nothing read for a millisecond: the next packet goes on where it ended.
    sleep_ms(2);
    i2c_read(buf, 32);
    CHECK(buf[0] != '$' || memcmp(buf, "$GNGGA", 6) != 0);
    // A query and the GNSS search mode, written whole.
    i2c_write((const uint8_t *)query, strlen(query));
    i2c_write((const uint8_t *)gps, strlen(gps));
    sleep_ms(3);
    CHECK(get1("gnss") == PA1010D_GNSS_GPS);
    CHECK(get1("commands") == 2);
    // The replies follow the rest of the epoch.
    pa1010d_collect(text, sizeof(text), 6);
    CHECK(strstr(text, "$PMTK514,0,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0*") != NULL);
    CHECK(strstr(text, "$PMTK001,353,3*") != NULL);
    CHECK(strstr(text, "$GNVTG,") != NULL);
    // The next epoch, GPS alone: GP talkers only, the replies gone.
    sleep_ms(1000);
    i2c_read(buf, 255);
    buf[255] = 0;
    CHECK(memcmp(buf, "$GPGGA,", 7) == 0);
    CHECK(strstr((const char *)buf, "$GL") == NULL);
    // Bad checksum: ignored; an unknown packet type: unsupported.
    i2c_write((const uint8_t *)"$PMTK220,200*2D\r\n", 17);
    i2c_write((const uint8_t *)"$PMTK999*3B\r\n", 13);
    sleep_ms(3);
    CHECK(get1("rate_ms") == 1000);
    CHECK(get1("commands") == 3);
}

static void behaviours_lcd(void) {
    const uint8_t text[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    uint8_t cells[80];
    const uint8_t cr[] = { 'X', '\r', 'Y' };
    const uint8_t cursor[] = { 0xfe, 0x47, 18, 1 };

    attach(&mclw_model_lcd_backpack);
    CHECK(get1("contrast") == 200 && get1("columns") == 16 && get1("rows") == 2);
    // 36 characters on 16x2 without autoscroll: past the end, the top left.
    uart_write(text, 36);
    CHECK(instance.model->ops->read_region(&instance, 0, 0, cells, 80) == 0);
    CHECK(memcmp(cells, "6789efghijklmnop", 16) == 0);
    CHECK(memcmp(&cells[20], "qrstuvwxyz0123", 14) == 0);
    // CR fills the row and moves down; a column past the end wraps a row.
    uart_write((const uint8_t[]){ 0xfe, 0x58 }, 2);
    uart_write(cr, 3);
    CHECK(instance.model->ops->read_region(&instance, 0, 0, cells, 80) == 0);
    CHECK(cells[0] == 'X' && cells[1] == ' ' && cells[20] == 'Y');
    uart_write(cursor, 4);
    CHECK(get1("cursor") == 2);
    // Autoscroll moves the rows up.
    uart_write((const uint8_t[]){ 0xfe, 0x51, 0xfe, 0x58 }, 4);
    uart_write(text, 33);
    CHECK(instance.model->ops->read_region(&instance, 0, 0, cells, 80) == 0);
    CHECK(memcmp(cells, "qrstuvwxyz012345", 16) == 0 && cells[20] == '6' && cells[21] == ' ');
    // Baud rate change and the test command's answer.
    uart_write((const uint8_t[]){ 0xfe, 0x39, 0x33 }, 3);
    CHECK(uart.baud == 19200 && get1("baud") == 19200);
    uart_write((const uint8_t[]){ 0xfe, 0xd2 }, 2);
    {
        char said[16];
        uart_take(said, sizeof(said));
        CHECK(strcmp(said, "19200") == 0);
    }
    // A power cycle keeps the saved settings; factory restores them.
    CHECK(instance.model->ops->reset(&instance, false) == 0);
    CHECK(get1("baud") == 19200 && get1("autoscroll") == 1);
    CHECK(instance.model->ops->reset(&instance, true) == 0);
    CHECK(get1("baud") == 9600 && get1("autoscroll") == 0);
}

static void behaviours_mpu9250(void) {
    uint8_t mosi[8];
    uint8_t miso[8];

    attach(&mclw_model_mpu9250);
    set_property("temperature=21000");
    set_property("acceleration=1000,0,-1000");
    mosi[0] = 0x80 | 0x41;
    spi_window(mosi, miso, 3);
    CHECK(miso[1] == 0 && miso[2] == 0);
    // ACCEL_FS_SEL 3 (+-16 g): 2048 LSB/g.
    mosi[0] = 0x1c;
    mosi[1] = 3u << 3;
    spi_window(mosi, miso, 2);
    mosi[0] = 0x80 | 0x3b;
    spi_window(mosi, miso, 7);
    CHECK(miso[1] == 0x08 && miso[2] == 0x00 && miso[5] == 0xf8 && miso[6] == 0x00);
    // H_RESET restores PWR_MGMT_1 and the full scale; WHO_AM_I holds.
    mosi[0] = 0x6b;
    mosi[1] = 0x80;
    spi_window(mosi, miso, 2);
    mosi[0] = 0x80 | 0x6b;
    spi_window(mosi, miso, 2);
    CHECK(miso[1] == 0x01);
    mosi[0] = 0x80 | 0x1c;
    spi_window(mosi, miso, 2);
    CHECK(miso[1] == 0x00);
    mosi[0] = 0x75;
    mosi[1] = 0x12;
    spi_window(mosi, miso, 2);
    mosi[0] = 0x80 | 0x75;
    spi_window(mosi, miso, 2);
    CHECK(miso[1] == 0x71);
}

static void behaviours_w25q(void) {
    static uint8_t mosi[4 + 256];
    static uint8_t miso[4 + 256];

    attach(&mclw_model_w25q16jv);
    // Program without a write enable: ignored, and the BUSY the engine set
    // at the release goes once the model has taken the window.
    memset(mosi, 0, sizeof(mosi));
    mosi[0] = 0x02;
    spi_window(mosi, miso, 4 + 16);
    mosi[0] = 0x05;
    spi_window(mosi, miso, 2);
    CHECK((miso[1] & 0x01) == 0x01);
    spi_window(mosi, miso, 2);
    CHECK(miso[1] == 0x00);
    CHECK(get1("ignored") == 1 && get1("programs") == 0);
    // A write enable then a program wrapping in its page: bytes from 0xf8
    // run on to 0x00.
    mosi[0] = 0x06;
    spi_window(mosi, miso, 1);
    mosi[0] = 0x02;
    mosi[1] = 0x00;
    mosi[2] = 0x01;
    mosi[3] = 0xf8;
    for (int i = 0; i < 16; i++) {
        mosi[4 + i] = (uint8_t)(0xa0 + i);
    }
    spi_window(mosi, miso, 4 + 16);
    spi_settle();
    CHECK(get1("busy") == 1 && get1("write_enabled") == 1);
    sleep_ms(2);
    CHECK(get1("busy") == 0 && get1("write_enabled") == 0);
    flash_read(0x01f8, miso, 8);
    CHECK(miso[0] == 0xa0 && miso[7] == 0xa7);
    flash_read(0x0100, miso, 8);
    CHECK(miso[0] == 0xa8 && miso[7] == 0xaf);
    // Programming only clears bits.
    mosi[0] = 0x06;
    spi_window(mosi, miso, 1);
    memset(mosi, 0, sizeof(mosi));
    mosi[0] = 0x02;
    mosi[1] = 0x00;
    mosi[2] = 0x01;
    mosi[3] = 0x00;
    mosi[4] = 0x0f;
    spi_window(mosi, miso, 5);
    spi_settle();
    sleep_ms(2);
    flash_read(0x0100, miso, 1);
    CHECK(miso[0] == (0xa8 & 0x0f));
    // Past the held pages nothing is kept; an erase there still takes tSE.
    mosi[0] = 0x06;
    spi_window(mosi, miso, 1);
    mosi[0] = 0x02;
    mosi[1] = 0x00;
    mosi[2] = 0x10;
    mosi[3] = 0x00;
    mosi[4] = 0x00;
    spi_window(mosi, miso, 5);
    spi_settle();
    sleep_ms(2);
    CHECK(get1("lost") == 1);
    flash_read(0x0700, miso, 4);
    CHECK(miso[0] == 0xff && miso[3] == 0xff);
    // While BUSY every instruction but Read Status is ignored.
    flash_write_enable();
    mosi[0] = 0x20;
    mosi[1] = 0x00;
    mosi[2] = 0x00;
    mosi[3] = 0x00;
    spi_window(mosi, miso, 4);
    spi_settle();
    flash_write_enable();
    spi_settle();
    CHECK(get1("busy") == 1 && get1("ignored") == 2);
    sleep_ms(46);
    CHECK(get1("busy") == 0);
    flash_read(0x0100, miso, 4);
    CHECK(miso[0] == 0xff);
}

int main(int argc, char **argv) {
    CHECK(argc >= 2);
    if (strcmp(argv[1], "behaviours") == 0) {
        behaviours_neo_m8n();
        memset(&uart, 0, sizeof(uart));
        behaviours_pa1010d();
        memset(&uart, 0, sizeof(uart));
        behaviours_lcd();
        behaviours_mpu9250();
        memset(&spi, 0, sizeof(spi));
        behaviours_w25q();
        printf("ok\n");
        return 0;
    }
    CHECK(argc >= 3);
    const struct mclw_model *models[] = { &mclw_model_neo_m8n, &mclw_model_pa1010d, &mclw_model_lcd_backpack,
        &mclw_model_mpu9250, &mclw_model_w25q16jv };
    const struct mclw_model *model = NULL;
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        if (strcmp(models[i]->id, argv[2]) == 0) {
            model = models[i];
        }
    }
    CHECK(model != NULL);
    attach(model);
    for (int i = 3; i < argc; i++) {
        set_property(argv[i]);
    }
    if (strcmp(argv[1], "gps_uart") == 0) {
        gps_uart_program();
    } else if (strcmp(argv[1], "pa1010d_i2c") == 0) {
        pa1010d_program();
    } else if (strcmp(argv[1], "lcd_uart") == 0) {
        lcd_uart_program();
    } else if (strcmp(argv[1], "mpu9250_spi") == 0) {
        mpu9250_program();
    } else if (strcmp(argv[1], "spi_flash") == 0) {
        flash_program(false);
    } else if (strcmp(argv[1], "pio_spi_flash") == 0) {
        flash_program(true);
    } else {
        printf("unknown target %s\n", argv[1]);
        return 1;
    }
    print_part();
    return 0;
}
