// SPDX-License-Identifier: Apache-2.0 OR MIT
// Runs a wire part model on the host behind stand-ins for the wire engines
// (core/ports/rp2/hal/wire.c): what an engine would capture is queued from
// stdin and handed out by read() as the engine does (a LATCHED_IN capture
// whole, once armed); what the model makes the engine do is printed. The
// 1-Wire layer is the real one (core/src/onewire.c), driven slot by slot.
// One command a line:
//
//   q <binding> <hex>...   queue entries for a binding's read()
//   p                      the worker's poll
//   t <us>                 the clock moves on, time_advanced each ms
//   r                      a responder played its waveform once
//   d <n>                  a STEPS_OUT engine put out n words
//   l <binding> <level>    the level on a binding's first pin now
//   v <binding>            the binding's engine lost entries (an overrun)
//   o reset                a 1-Wire reset and presence
//   o <slots>              1-Wire slots, a character each: what the master
//                          puts on the line ('0' holds it low, '1' leaves it
//                          to the pull-up); prints what the line read
//
// and at the end, a line per property ("prop <name> <type> <value>...") and
// per region ("region <name> <hex>"), then what the engines were given:
// "pulses <hex>...", "words <hex>...", "period <ns>", "captures <n>".
//
//   wire_models_check <model> [binding=pin,pin...]... [property=value[,value]]...
//
// A binding given as `binding=-` is left out.

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/wire.h"
#include "metalclaw/model.h"
#include "metalclaw/onewire.h"
#include "metalclaw/runtime.h"

#include "apa102.h"
#include "dht22.h"
#include "ds18b20.h"
#include "hub75.h"
#include "logic_capture.h"
#include "pwm_capture.h"
#include "quadrature_encoder.h"
#include "sn754410.h"
#include "tsop382.h"
#include "ws2812b.h"

#define QUEUE_MAX 65536u
#define WORDS_MAX 4096u
#define FIFO_WORDS 8u

struct mclw_wire {
    const struct mclw_wire_facts *facts;
    uint8_t pins[MCLW_WIRE_PINS_MAX];
    uint8_t count;
    uint32_t queue[QUEUE_MAX];
    size_t head;
    size_t tail;
    // LATCHED_IN: captures armed, and the length of each queued capture.
    uint32_t armed;
    size_t capture_lengths[512];
    size_t captures;
    size_t captures_taken;
    uint32_t pulses[256];
    size_t pulse_count;
    uint32_t responses;
    uint32_t period_ns;
    uint32_t words[WORDS_MAX];
    size_t word_count;
    size_t fifo;
    struct mclw_onewire *onewire;
    int level;
    uint32_t overruns;
};

static struct mclw_wire wires[MCLW_INSTANCE_BINDINGS_MAX];
static struct mclw_instance instance;
static uint64_t now_us;
static uint64_t ticked_ms;
// What the 1-Wire part drives in the next slot: false holds the line low.
static bool part_drive = true;

uint32_t mclw_hal_critical_enter(void) {
    return 0;
}

void mclw_hal_critical_exit(uint32_t state) {
    (void)state;
}

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

struct mclw_instance *mclw_instance_from_state(const void *state) {
    return state == instance.state ? &instance : NULL;
}

uint8_t mclw_hal_wire_pin(const struct mclw_wire *w, uint8_t index) {
    return index < w->count ? w->pins[index] : 0xffu;
}

int mclw_hal_wire_sample_bit(const struct mclw_wire *w, uint8_t pin) {
    return w->facts->engine == MCLW_WIRE_LATCHED_IN && pin < 32u ? (int)pin : -1;
}

void mclw_hal_wire_stats(const struct mclw_wire *w, struct mclw_wire_stats *out) {
    memset(out, 0, sizeof(*out));
    out->overruns = w->overruns;
}

int mclw_hal_wire_level(const struct mclw_wire *w, uint8_t index) {
    return index < w->count ? w->level : -1;
}

size_t mclw_hal_wire_capture_words(const struct mclw_wire *w) {
    (void)w;
    return HUB75_CAPTURE_WORDS;
}

int mclw_hal_wire_capture(struct mclw_wire *w) {
    w->armed++;
    return 0;
}

size_t mclw_hal_wire_read(struct mclw_wire *w, uint32_t *out, size_t max) {
    size_t n = 0;

    if (w->facts->engine == MCLW_WIRE_LATCHED_IN) {
        if (w->armed == 0u || w->captures_taken == w->captures) {
            return 0;
        }
        size_t len = w->capture_lengths[w->captures_taken++];
        w->armed--;
        if (len > max) {
            w->head += len;
            return 0;
        }
        memcpy(out, &w->queue[w->head], len * sizeof(out[0]));
        w->head += len;
        return len;
    }
    while (n < max && w->head < w->tail) {
        out[n++] = w->queue[w->head++];
    }
    return n;
}

int mclw_hal_wire_respond(struct mclw_wire *w, const uint32_t *pulses, size_t count) {
    if (count > sizeof(w->pulses) / sizeof(w->pulses[0])) {
        return -EINVAL;
    }
    memcpy(w->pulses, pulses, count * sizeof(pulses[0]));
    w->pulse_count = count;
    return 0;
}

uint32_t mclw_hal_wire_responses(struct mclw_wire *w) {
    return w->responses;
}

bool mclw_hal_wire_idle(struct mclw_wire *w) {
    return w->fifo == 0u;
}

int mclw_hal_wire_set_period(struct mclw_wire *w, uint32_t ns) {
    if (w->fifo != 0u) {
        return -EBUSY;
    }
    w->period_ns = ns;
    return 0;
}

size_t mclw_hal_wire_write(struct mclw_wire *w, const uint32_t *words, size_t count) {
    size_t n = 0;

    while (n < count && w->fifo < FIFO_WORDS && w->word_count < WORDS_MAX) {
        w->words[w->word_count++] = words[n++];
        w->fifo++;
    }
    return n;
}

int mclw_hal_wire_onewire_arm(struct mclw_wire *w, struct mclw_onewire *image) {
    w->onewire = image;
    return 0;
}

static const struct mclw_model *const models[] = {
    &mclw_model_ws2812b,
    &mclw_model_apa102,
    &mclw_model_ds18b20,
    &mclw_model_dht22,
    &mclw_model_quadrature_encoder,
    &mclw_model_tsop38238,
    &mclw_model_hub75,
    &mclw_model_logic_capture,
    &mclw_model_pwm_capture,
    &mclw_model_sn754410,
};

static void fail(const char *what) {
    printf("failed: %s\n", what);
    exit(1);
}

static int binding_index(const char *name) {
    for (size_t i = 0; i < instance.model->binding_count; i++) {
        if (strcmp(instance.model->bindings[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int property_index(const char *name) {
    for (size_t i = 0; i < instance.model->property_count; i++) {
        if (strcmp(instance.model->properties[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static const char *type_name(enum mclw_value_type type) {
    return type == MCLW_TYPE_BOOL ? "bool" : type == MCLW_TYPE_I32 ? "i32" : "u32";
}

static struct mclw_wire *wire_named(const char *name) {
    int index = binding_index(name);
    if (index < 0) {
        fail("no such binding");
    }
    return &wires[index];
}

static void advance(uint64_t us) {
    uint64_t end = now_us + us;

    while (now_us < end) {
        uint64_t step = end - now_us < 1000u ? end - now_us : 1000u;
        now_us += step;
        if (now_us / 1000u != ticked_ms) {
            ticked_ms = now_us / 1000u;
            if (instance.model->ops->time_advanced != NULL) {
                instance.model->ops->time_advanced(&instance, now_us);
            }
            if (instance.model->ops->poll != NULL) {
                instance.model->ops->poll(&instance);
            }
        }
    }
}

static struct mclw_onewire *onewire_image(void) {
    for (size_t i = 0; i < MCLW_INSTANCE_BINDINGS_MAX; i++) {
        if (wires[i].onewire != NULL) {
            return wires[i].onewire;
        }
    }
    fail("no 1-Wire image armed");
    return NULL;
}

// Each slot's level is the master's AND the part's; the part samples it
// and says what it does in the next.
static void onewire_slots(const char *slots) {
    struct mclw_onewire *ow = onewire_image();

    printf("line ");
    for (const char *c = slots; *c != '\0'; c++) {
        bool level = *c == '1' && part_drive;
        putchar(level ? '1' : '0');
        part_drive = mclw_onewire_slot(ow, level);
    }
    putchar('\n');
}

static void command(char *line) {
    char *tok = strtok(line, " \t\n");

    if (tok == NULL) {
        return;
    }
    if (strcmp(tok, "q") == 0) {
        struct mclw_wire *w = wire_named(strtok(NULL, " \t\n"));
        size_t start = w->tail;
        for (char *v = strtok(NULL, " \t\n"); v != NULL; v = strtok(NULL, " \t\n")) {
            if (w->tail >= QUEUE_MAX) {
                fail("queue full");
            }
            w->queue[w->tail++] = (uint32_t)strtoul(v, NULL, 16);
        }
        if (w->facts->engine == MCLW_WIRE_LATCHED_IN) {
            if (w->captures >= sizeof(w->capture_lengths) / sizeof(w->capture_lengths[0])) {
                fail("too many captures");
            }
            w->capture_lengths[w->captures++] = w->tail - start;
        }
    } else if (strcmp(tok, "p") == 0) {
        instance.model->ops->poll(&instance);
    } else if (strcmp(tok, "t") == 0) {
        advance(strtoull(strtok(NULL, " \t\n"), NULL, 0));
    } else if (strcmp(tok, "r") == 0) {
        for (size_t i = 0; i < MCLW_INSTANCE_BINDINGS_MAX; i++) {
            wires[i].responses++;
        }
    } else if (strcmp(tok, "d") == 0) {
        size_t n = strtoul(strtok(NULL, " \t\n"), NULL, 0);
        for (size_t i = 0; i < MCLW_INSTANCE_BINDINGS_MAX; i++) {
            wires[i].fifo = wires[i].fifo > n ? wires[i].fifo - n : 0;
        }
    } else if (strcmp(tok, "l") == 0) {
        struct mclw_wire *w = wire_named(strtok(NULL, " \t\n"));
        w->level = (int)strtol(strtok(NULL, " \t\n"), NULL, 0);
    } else if (strcmp(tok, "v") == 0) {
        wire_named(strtok(NULL, " \t\n"))->overruns++;
    } else if (strcmp(tok, "o") == 0) {
        char *what = strtok(NULL, " \t\n");
        if (strcmp(what, "reset") == 0) {
            mclw_onewire_reset(onewire_image());
            part_drive = true;
        } else {
            onewire_slots(what);
        }
    } else {
        fail("unknown command");
    }
}

static void print_state(void) {
    const struct mclw_model *m = instance.model;

    for (size_t i = 0; i < m->property_count; i++) {
        struct mclw_value v = { 0 };
        v.count = m->properties[i].count;
        if (m->ops->get(&instance, i, &v) != 0) {
            fail("get");
        }
        printf("prop %s %s", m->properties[i].name, type_name(m->properties[i].type));
        for (size_t k = 0; k < v.count; k++) {
            printf(" %lld", (long long)v.v[k]);
        }
        printf("\n");
    }
    for (size_t i = 0; i < m->region_count; i++) {
        static uint8_t buf[8192];
        if (m->regions[i].size > sizeof(buf) || m->ops->read_region(&instance, i, 0, buf, m->regions[i].size) != 0) {
            fail("read_region");
        }
        printf("region %s ", m->regions[i].name);
        for (size_t k = 0; k < m->regions[i].size; k++) {
            printf("%02x", buf[k]);
        }
        printf("\n");
    }
    for (size_t b = 0; b < m->binding_count; b++) {
        const struct mclw_wire *w = &wires[b];
        if (w->pulse_count > 0u) {
            printf("pulses");
            for (size_t i = 0; i < w->pulse_count; i++) {
                printf(" %08x", (unsigned)w->pulses[i]);
            }
            printf("\n");
        }
        if (w->word_count > 0u) {
            printf("words");
            for (size_t i = 0; i < w->word_count; i++) {
                printf(" %08x", (unsigned)w->words[i]);
            }
            printf("\nperiod %u\n", (unsigned)w->period_ns);
        }
        if (w->facts->engine == MCLW_WIRE_LATCHED_IN) {
            printf("captures %u\n", (unsigned)w->armed);
        }
    }
}

int main(int argc, char **argv) {
    struct mclw_binding bindings[MCLW_INSTANCE_BINDINGS_MAX];
    static char line[1u << 20];

    if (argc < 2) {
        fail("usage");
    }
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]) && instance.model == NULL; i++) {
        if (strcmp(models[i]->id, argv[1]) == 0) {
            instance.model = models[i];
        }
    }
    if (instance.model == NULL) {
        fail("no such model");
    }
    instance.state = calloc(1, instance.model->state_size);
    memset(bindings, 0, sizeof(bindings));
    for (size_t b = 0; b < instance.model->binding_count; b++) {
        const struct mclw_binding_desc *d = &instance.model->bindings[b];
        wires[b].facts = &d->wire;
        bindings[b].kind = d->kind;
        bindings[b].present = true;
        bindings[b].wire = &wires[b];
        for (uint8_t i = 0; i < d->wire.pins; i++) {
            wires[b].pins[i] = (uint8_t)(b * 4u + i);
        }
        wires[b].count = d->wire.pins;
        wires[b].level = -1;
    }
    for (int i = 2; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL) {
            fail("argument");
        }
        *eq = '\0';
        int b = binding_index(argv[i]);
        if (b >= 0 && strcmp(eq + 1, "-") == 0) {
            bindings[b].present = false;
            bindings[b].wire = NULL;
        } else if (b >= 0) {
            wires[b].count = 0;
            for (char *p = strtok(eq + 1, ","); p != NULL; p = strtok(NULL, ",")) {
                wires[b].pins[wires[b].count++] = (uint8_t)strtoul(p, NULL, 0);
            }
        }
    }
    if (instance.model->ops->init(&instance, bindings) != 0) {
        fail("init");
    }
    for (int i = 2; i < argc; i++) {
        int b = binding_index(argv[i]);
        int p = property_index(argv[i]);
        if (b >= 0) {
            continue;
        }
        if (p < 0) {
            fail("no such property");
        }
        struct mclw_value v = { 0 };
        const char *value = argv[i] + strlen(argv[i]) + 1;
        char copy[256];
        snprintf(copy, sizeof(copy), "%s", value);
        for (char *t = strtok(copy, ","); t != NULL && v.count < MCLW_VALUE_MAX_COUNT; t = strtok(NULL, ",")) {
            v.v[v.count++] = strtoll(t, NULL, 0);
        }
        if (instance.model->ops->set(&instance, (size_t)p, &v) != 0) {
            fail("set");
        }
    }
    while (fgets(line, sizeof(line), stdin) != NULL) {
        command(line);
    }
    print_state();
    return 0;
}
