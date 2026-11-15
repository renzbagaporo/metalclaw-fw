// SPDX-License-Identifier: Apache-2.0 OR MIT
// Runs a display model on the host behind stand-ins for the sink engines
// (core/ports/rp2/hal/i2c_target.c and spi_sink.c): every byte a host sends
// reaches the model's sink in order, at most 64 to a delivery as the SPI
// engine batches them, the first of each transaction or chip-select window
// marked MCLW_SINK_FIRST, an SPI byte sent with D/C high marked
// MCLW_SINK_LINE, and the window reported ended after its last byte. What
// the host sends comes on stdin, one transfer a line:
//
//   i <hex>...        an I2C write transaction
//   s <dc> <hex>...   an SPI window with D/C at <dc> throughout
//   r <level>         the reset pin goes to <level>
//   p                 the worker's poll runs
//
// and what the model then holds comes out on stdout: a line per property
// ("prop <name> <type> <value>..."), then a line per region
// ("region <name> <hex>").
//
//   display_models_check <model> <i2c|spi> [reset] [property=value]...

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_sink.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"
#include "metalclaw/sink.h"

#include "hd44780.h"
#include "ht16k33.h"
#include "max7219.h"
#include "ssd130x.h"
#include "st7789v.h"

#define BATCH 64u
#define LINE_MAX 65536

struct mclw_i2c_target {
    struct mclw_sink *sink;
};

struct mclw_spi_sink {
    struct mclw_sink *sink;
};

struct mclw_gpio {
    bool level;
    bool fell;
};

static struct mclw_i2c_target i2c;
static struct mclw_spi_sink spi;
static struct mclw_gpio reset_pin = { .level = true, .fell = false };
static struct mclw_instance instance;

uint32_t mclw_hal_critical_enter(void) {
    return 0;
}

void mclw_hal_critical_exit(uint32_t state) {
    (void)state;
}

int mclw_hal_i2c_target_arm_sink(struct mclw_i2c_target *t, struct mclw_sink *sink) {
    t->sink = sink;
    return 0;
}

void mclw_hal_i2c_target_disarm(struct mclw_i2c_target *t) {
    t->sink = NULL;
}

int mclw_hal_spi_sink_arm(struct mclw_spi_sink *s, struct mclw_sink *sink) {
    s->sink = sink;
    return 0;
}

void mclw_hal_spi_sink_disarm(struct mclw_spi_sink *s) {
    s->sink = NULL;
}

bool mclw_hal_gpio_read(const struct mclw_gpio *gpio) {
    return gpio->level;
}

bool mclw_hal_gpio_fell(struct mclw_gpio *gpio) {
    bool fell = gpio->fell;
    gpio->fell = false;
    return fell;
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

static const struct mclw_model *const models[] = {
    &mclw_model_ssd1306,
    &mclw_model_ssd1309,
    &mclw_model_ht16k33,
    &mclw_model_hd44780_pcf8574,
    &mclw_model_max7219,
    &mclw_model_st7789v,
};

static void fail(const char *what) {
    printf("failed: %s\n", what);
    exit(1);
}

static void deliver(struct mclw_sink *sink, const uint16_t *entries, size_t count) {
    if (sink == NULL) {
        fail("no sink armed");
    }
    for (size_t at = 0; at < count; at += BATCH) {
        size_t n = count - at < BATCH ? count - at : BATCH;
        sink->on_bytes(sink, &entries[at], n, at + n == count, sink->ctx);
    }
}

static size_t parse_bytes(char *text, uint16_t *entries, uint16_t flags) {
    size_t n = 0;

    for (char *tok = strtok(text, " \t\n"); tok != NULL; tok = strtok(NULL, " \t\n")) {
        entries[n] = (uint16_t)(strtoul(tok, NULL, 16) & 0xffu) | flags;
        if (n == 0) {
            entries[n] |= MCLW_SINK_FIRST;
        }
        n++;
    }
    return n;
}

static void set_property(const char *assignment) {
    const struct mclw_model *m = instance.model;
    const char *eq = strchr(assignment, '=');
    struct mclw_value v = { 0 };

    if (eq == NULL) {
        fail("property=value");
    }
    for (size_t i = 0; i < m->property_count; i++) {
        if (strncmp(m->properties[i].name, assignment, (size_t)(eq - assignment)) == 0 &&
            m->properties[i].name[eq - assignment] == '\0') {
            v.count = 1;
            v.v[0] = strtoll(eq + 1, NULL, 0);
            if (m->ops->set == NULL || m->ops->set(&instance, i, &v) != 0) {
                fail(assignment);
            }
            return;
        }
    }
    fail(assignment);
}

static void dump(void) {
    const struct mclw_model *m = instance.model;
    static uint8_t buf[4096];

    for (size_t i = 0; i < m->property_count; i++) {
        struct mclw_value v = { 0 };

        v.count = m->properties[i].count;
        if (m->ops->get(&instance, i, &v) != 0) {
            fail(m->properties[i].name);
        }
        printf("prop %s %s", m->properties[i].name,
            m->properties[i].type == MCLW_TYPE_BOOL ? "bool" : m->properties[i].type == MCLW_TYPE_I32 ? "i32" : "u32");
        for (uint8_t k = 0; k < v.count; k++) {
            printf(" %lld", (long long)v.v[k]);
        }
        printf("\n");
    }
    for (size_t i = 0; i < m->region_count; i++) {
        uint32_t size = m->regions[i].size;

        if (size > sizeof(buf) || m->ops->read_region(&instance, i, 0, buf, size) != 0) {
            fail(m->regions[i].name);
        }
        printf("region %s ", m->regions[i].name);
        for (uint32_t k = 0; k < size; k++) {
            printf("%02x", buf[k]);
        }
        printf("\n");
    }
}

int main(int argc, char **argv) {
    static char line[LINE_MAX];
    static uint16_t entries[LINE_MAX / 2];
    struct mclw_binding bindings[MCLW_INSTANCE_BINDINGS_MAX];
    int arg = 3;

    if (argc < 3) {
        fail("usage: display_models_check <model> <i2c|spi> [reset] [property=value]...");
    }
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        if (strcmp(models[i]->id, argv[1]) == 0) {
            instance.model = models[i];
        }
    }
    if (instance.model == NULL) {
        fail("no such model");
    }
    memset(bindings, 0, sizeof(bindings));
    bool with_reset = argc > 3 && strcmp(argv[3], "reset") == 0;
    arg += with_reset ? 1 : 0;
    for (size_t i = 0; i < instance.model->binding_count; i++) {
        const struct mclw_binding_desc *b = &instance.model->bindings[i];

        bindings[i].kind = b->kind;
        if (strcmp(b->name, argv[2]) == 0 && b->kind == MCLW_BINDING_I2C_TARGET) {
            bindings[i].i2c = &i2c;
            bindings[i].present = true;
        } else if (strcmp(b->name, argv[2]) == 0 && b->kind == MCLW_BINDING_SPI_SINK) {
            bindings[i].spi_sink = &spi;
            bindings[i].present = true;
        } else if (with_reset && strcmp(b->name, "reset") == 0) {
            bindings[i].gpio = &reset_pin;
            bindings[i].present = true;
        }
    }
    instance.state = calloc(1, instance.model->state_size);
    if (instance.state == NULL || instance.model->ops->init(&instance, bindings) != 0) {
        fail("init");
    }
    for (; arg < argc; arg++) {
        set_property(argv[arg]);
    }

    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t n;

        switch (line[0]) {
            case 'i':
                n = parse_bytes(line + 1, entries, 0);
                deliver(i2c.sink, entries, n);
                break;
            case 's': {
                char *rest = NULL;
                long dc = strtol(line + 1, &rest, 10);
                n = parse_bytes(rest, entries, dc != 0 ? MCLW_SINK_LINE : 0);
                deliver(spi.sink, entries, n);
                break;
            }
            case 'r': {
                bool level = strtol(line + 1, NULL, 10) != 0;
                if (reset_pin.level && !level) {
                    reset_pin.fell = true;
                }
                reset_pin.level = level;
                break;
            }
            case 'p':
                if (instance.model->ops->poll != NULL) {
                    instance.model->ops->poll(&instance);
                }
                break;
            default:
                break;
        }
    }
    dump();
    return 0;
}
