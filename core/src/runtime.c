// SPDX-License-Identifier: Apache-2.0 OR MIT

#include <errno.h>
#include <string.h>

#include "metalclaw/runtime.h"
#include "metalclaw/hal/gpio.h"
#include "metalclaw/hal/i2c_target.h"
#include "metalclaw/hal/pins.h"
#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/spi_target.h"
#include "metalclaw/hal/swd.h"
#include "metalclaw/hal/uart.h"
#include "metalclaw/hal/usb.h"

#ifndef METALCLAW_INSTANCES_MAX
#define METALCLAW_INSTANCES_MAX 8
#endif
#ifndef METALCLAW_ARENA_SIZE
#define METALCLAW_ARENA_SIZE 8192
#endif

static struct metalclaw_instance instances[METALCLAW_INSTANCES_MAX];
static uint8_t arena[METALCLAW_ARENA_SIZE] __attribute__((aligned(8)));
// The arena is handed out in 64-byte pages, first fit, so a detached
// instance's state is reusable at once.
#define ARENA_PAGE 64u
#define ARENA_PAGES (METALCLAW_ARENA_SIZE / ARENA_PAGE)
static uint8_t arena_owner[ARENA_PAGES];
static size_t attached_count;
static uint64_t clock_us;
static bool clock_paused;
static uint64_t clock_last_real_us;

#define CLOCK_TICK_US 1000u

const struct metalclaw_model *metalclaw_model_find(const char *id) {
    for (size_t i = 0; i < metalclaw_model_count; i++) {
        if (strcmp(metalclaw_models[i]->id, id) == 0) {
            return metalclaw_models[i];
        }
    }
    return NULL;
}

static int index_of(const char *const *names, size_t stride_bytes, size_t count, const char *name) {
    const uint8_t *base = (const uint8_t *)names;
    for (size_t i = 0; i < count; i++) {
        const char *candidate = *(const char *const *)(base + i * stride_bytes);
        if (strcmp(candidate, name) == 0) {
            return (int)i;
        }
    }
    return -ENOENT;
}

int metalclaw_property_index(const struct metalclaw_model *model, const char *name) {
    return index_of(&model->properties[0].name, sizeof(model->properties[0]), model->property_count, name);
}

int metalclaw_region_index(const struct metalclaw_model *model, const char *name) {
    return index_of(&model->regions[0].name, sizeof(model->regions[0]), model->region_count, name);
}

int metalclaw_fault_index(const struct metalclaw_model *model, const char *name) {
    return index_of(model->faults, sizeof(model->faults[0]), model->fault_count, name);
}

int metalclaw_binding_index(const struct metalclaw_model *model, const char *name) {
    return index_of(&model->bindings[0].name, sizeof(model->bindings[0]), model->binding_count, name);
}

struct metalclaw_instance *metalclaw_instance_find(const char *name) {
    for (size_t i = 0; i < METALCLAW_INSTANCES_MAX; i++) {
        if (instances[i].attached && strcmp(instances[i].name, name) == 0) {
            return &instances[i];
        }
    }
    return NULL;
}

struct metalclaw_instance *metalclaw_instance_from_state(const void *state) {
    const uint8_t *p = state;

    if (p < arena || p >= arena + METALCLAW_ARENA_SIZE) {
        return NULL;
    }
    uint8_t tag = arena_owner[(size_t)(p - arena) / ARENA_PAGE];
    return tag == 0 ? NULL : &instances[tag - 1u];
}

size_t metalclaw_instance_count(void) {
    return attached_count;
}

struct metalclaw_instance *metalclaw_instance_at(size_t index) {
    for (size_t i = 0; i < METALCLAW_INSTANCES_MAX; i++) {
        if (instances[i].attached) {
            if (index == 0) {
                return &instances[i];
            }
            index--;
        }
    }
    return NULL;
}

static void release_bindings(struct metalclaw_instance *inst) {
    for (size_t i = 0; i < METALCLAW_INSTANCE_BINDINGS_MAX; i++) {
        struct metalclaw_binding *b = &inst->bindings[i];
        if (!b->present) {
            continue;
        }
        switch (b->kind) {
            case METALCLAW_BINDING_GPIO_IN:
            case METALCLAW_BINDING_GPIO_OUT:
                metalclaw_hal_gpio_close(b->gpio);
                break;
            case METALCLAW_BINDING_SWD_WIRE:
                metalclaw_hal_swd_close(b->swd);
                break;
            case METALCLAW_BINDING_I2C_TARGET:
                metalclaw_hal_i2c_target_close(b->i2c);
                break;
            case METALCLAW_BINDING_SPI_TARGET:
                metalclaw_hal_spi_target_close(b->spi);
                break;
            case METALCLAW_BINDING_UART:
                metalclaw_hal_uart_close(b->uart);
                break;
            case METALCLAW_BINDING_USB_INTERFACE:
                metalclaw_hal_usb_close(b->usb);
                break;
            default:
                break;
        }
        b->present = false;
        b->handle = NULL;
    }
    metalclaw_hal_pin_release_all(inst);
}

static void release_arena(const struct metalclaw_instance *inst) {
    uint8_t tag = (uint8_t)(inst - instances) + 1u;

    for (size_t i = 0; i < ARENA_PAGES; i++) {
        if (arena_owner[i] == tag) {
            arena_owner[i] = 0;
        }
    }
}

struct usb_rx_context {
    struct metalclaw_instance *inst;
    size_t binding;
};

static void usb_rx_trampoline(struct metalclaw_usb_interface *itf, const uint8_t *data, size_t len, void *ctx) {
    (void)itf;
    struct usb_rx_context *c = ctx;
    metalclaw_hal_lock();
    if (c->inst->attached && c->inst->model->ops->usb_rx != NULL) {
        c->inst->model->ops->usb_rx(c->inst, c->binding, data, len);
    }
    metalclaw_hal_unlock();
}

static struct usb_rx_context usb_contexts[METALCLAW_INSTANCES_MAX][METALCLAW_INSTANCE_BINDINGS_MAX];

static int resolve_binding(struct metalclaw_instance *inst, size_t index, const struct metalclaw_binding_desc *desc,
    const struct metalclaw_binding_request *req, struct metalclaw_binding *out) {
    int err = 0;
    out->kind = desc->kind;
    out->present = false;
    out->handle = NULL;

    if (desc->kind == METALCLAW_BINDING_USB_INTERFACE) {
        if (desc->usb == NULL) {
            return -EINVAL;
        }
        struct usb_rx_context *ctx = &usb_contexts[inst - instances][index];
        ctx->inst = inst;
        ctx->binding = index;
        out->usb = metalclaw_hal_usb_open(desc->usb, usb_rx_trampoline, ctx, inst, &err);
        if (out->usb == NULL) {
            return err;
        }
        out->present = true;
        return 0;
    }
    if (!req->present) {
        return desc->required ? -EINVAL : 0;
    }
    switch (desc->kind) {
        case METALCLAW_BINDING_GPIO_IN:
        case METALCLAW_BINDING_GPIO_OUT:
            if (req->pin_count != 1) {
                return -EINVAL;
            }
            out->gpio = metalclaw_hal_gpio_open(req->pins[0], desc->kind == METALCLAW_BINDING_GPIO_OUT,
                METALCLAW_GPIO_PULL_NONE, false, inst, &err);
            if (out->gpio == NULL) {
                return err;
            }
            out->present = true;
            return 0;
        case METALCLAW_BINDING_I2C_TARGET:
            if (req->pin_count != 3) {
                return -EINVAL;
            }
            out->i2c = metalclaw_hal_i2c_target_open(req->pins[0], req->pins[1], req->pins[2], inst, &err);
            if (out->i2c == NULL) {
                return err;
            }
            out->present = true;
            return 0;
        case METALCLAW_BINDING_SPI_TARGET:
            if (req->pin_count != 4) {
                return -EINVAL;
            }
            out->spi = metalclaw_hal_spi_target_open(req->pins[0], req->pins[1], req->pins[2], req->pins[3],
                desc->spi.default_mode, desc->spi.msb_first, inst, &err);
            if (out->spi == NULL) {
                return err;
            }
            out->present = true;
            return 0;
        case METALCLAW_BINDING_UART:
            if (req->pin_count != 2) {
                return -EINVAL;
            }
            out->uart = metalclaw_hal_uart_open(req->pins[0], req->pins[1], desc->uart.default_baud,
                &desc->uart.format, inst, &err);
            if (out->uart == NULL) {
                return err;
            }
            out->present = true;
            return 0;
        case METALCLAW_BINDING_SWD_WIRE:
            if (req->pin_count != 2 && req->pin_count != 3) {
                return -EINVAL;
            }
            out->swd = metalclaw_hal_swd_open(req->pins[0], req->pins[1],
                req->pin_count == 3 ? req->pins[2] : METALCLAW_PIN_NONE, inst, &err);
            if (out->swd == NULL) {
                return err;
            }
            out->present = true;
            return 0;
        default:
            return -ENOTSUP;
    }
}

int metalclaw_attach(const struct metalclaw_model *model, const char *name,
    const struct metalclaw_binding_request *requests, struct metalclaw_instance **out) {
    if (model == NULL || name == NULL || out == NULL) {
        return -EINVAL;
    }
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > METALCLAW_INSTANCE_NAME_MAX) {
        return -EINVAL;
    }
    if (model->abi_version != METALCLAW_ABI_VERSION || model->binding_count > METALCLAW_INSTANCE_BINDINGS_MAX) {
        return -ENOTSUP;
    }
    metalclaw_hal_lock();
    if (metalclaw_instance_find(name) != NULL) {
        metalclaw_hal_unlock();
        return -EEXIST;
    }

    struct metalclaw_instance *inst = NULL;
    for (size_t i = 0; i < METALCLAW_INSTANCES_MAX; i++) {
        if (!instances[i].attached) {
            inst = &instances[i];
            break;
        }
    }
    size_t pages = (model->state_size + ARENA_PAGE - 1u) / ARENA_PAGE;
    size_t first = ARENA_PAGES;
    for (size_t i = 0, run = 0; inst != NULL && i < ARENA_PAGES; i++) {
        run = arena_owner[i] == 0 ? run + 1 : 0;
        if (run == pages) {
            first = i + 1 - pages;
            break;
        }
    }
    if (inst == NULL || (pages > 0 && first == ARENA_PAGES)) {
        metalclaw_hal_unlock();
        return -ENOSPC;
    }

    uint32_t generation = inst->generation + 1;
    memset(inst, 0, sizeof(*inst));
    inst->generation = generation;
    inst->model = model;
    memcpy(inst->name, name, name_len + 1);
    inst->state = pages > 0 ? &arena[first * ARENA_PAGE] : NULL;
    for (size_t i = 0; i < pages; i++) {
        arena_owner[first + i] = (uint8_t)(inst - instances) + 1u;
    }
    memset(inst->state, 0, pages * ARENA_PAGE);

    int err = 0;
    for (size_t i = 0; i < model->binding_count && err == 0; i++) {
        err = resolve_binding(inst, i, &model->bindings[i], &requests[i], &inst->bindings[i]);
    }
    if (err == 0) {
        err = model->ops->init(inst, inst->bindings);
    }
    if (err != 0) {
        release_bindings(inst);
        release_arena(inst);
        inst->model = NULL;
        metalclaw_hal_unlock();
        return err;
    }

    uint32_t state = metalclaw_hal_critical_enter();
    inst->attached = true;
    attached_count++;
    metalclaw_hal_critical_exit(state);
    metalclaw_hal_unlock();
    *out = inst;
    return 0;
}

int metalclaw_detach(struct metalclaw_instance *inst) {
    if (inst == NULL) {
        return -ENOENT;
    }
    metalclaw_hal_lock();
    if (!inst->attached) {
        metalclaw_hal_unlock();
        return -ENOENT;
    }
    uint32_t state = metalclaw_hal_critical_enter();
    inst->attached = false;
    attached_count--;
    metalclaw_hal_critical_exit(state);

    if (inst->model->ops->deinit != NULL) {
        inst->model->ops->deinit(inst);
    }
    release_bindings(inst);
    release_arena(inst);
    inst->model = NULL;
    inst->state = NULL;
    metalclaw_hal_unlock();
    return 0;
}

void metalclaw_detach_all(void) {
    for (size_t i = 0; i < METALCLAW_INSTANCES_MAX; i++) {
        if (instances[i].attached) {
            metalclaw_detach(&instances[i]);
        }
    }
}

static int check_value(const struct metalclaw_property_desc *desc, const struct metalclaw_value *in) {
    if (in->count != desc->count) {
        return -EINVAL;
    }
    for (size_t i = 0; i < in->count; i++) {
        int64_t v = in->v[i];
        if (v < desc->min || v > desc->max) {
            return -EINVAL;
        }
        switch (desc->type) {
            case METALCLAW_TYPE_BOOL:
                if (v != 0 && v != 1) {
                    return -EINVAL;
                }
                break;
            case METALCLAW_TYPE_I32:
                if (v < INT32_MIN || v > INT32_MAX) {
                    return -EINVAL;
                }
                break;
            case METALCLAW_TYPE_U32:
                if (v < 0 || v > UINT32_MAX) {
                    return -EINVAL;
                }
                break;
        }
    }
    return 0;
}

int metalclaw_get(struct metalclaw_instance *inst, size_t property, struct metalclaw_value *out) {
    if (inst == NULL || !inst->attached || out == NULL || property >= inst->model->property_count) {
        return -EINVAL;
    }
    const struct metalclaw_property_desc *desc = &inst->model->properties[property];
    if (desc->access == METALCLAW_ACCESS_WO || inst->model->ops->get == NULL) {
        return -ENOTSUP;
    }
    memset(out, 0, sizeof(*out));
    out->count = desc->count;
    metalclaw_hal_lock();
    int err = inst->model->ops->get(inst, property, out);
    metalclaw_hal_unlock();
    return err;
}

int metalclaw_set(struct metalclaw_instance *inst, size_t property, const struct metalclaw_value *in) {
    if (inst == NULL || !inst->attached || in == NULL || property >= inst->model->property_count) {
        return -EINVAL;
    }
    const struct metalclaw_property_desc *desc = &inst->model->properties[property];
    if (desc->access == METALCLAW_ACCESS_RO || inst->model->ops->set == NULL) {
        return -ENOTSUP;
    }
    int err = check_value(desc, in);
    if (err != 0) {
        return err;
    }
    metalclaw_hal_lock();
    err = inst->model->ops->set(inst, property, in);
    metalclaw_hal_unlock();
    return err;
}

int metalclaw_inject_fault(struct metalclaw_instance *inst, size_t fault, uint32_t count) {
    if (inst == NULL || !inst->attached || fault >= inst->model->fault_count) {
        return -EINVAL;
    }
    if (inst->model->ops->inject_fault == NULL) {
        return -ENOTSUP;
    }
    metalclaw_hal_lock();
    int err = inst->model->ops->inject_fault(inst, fault, count);
    metalclaw_hal_unlock();
    return err;
}

int metalclaw_clear_faults(struct metalclaw_instance *inst) {
    if (inst == NULL || !inst->attached) {
        return -EINVAL;
    }
    if (inst->model->ops->clear_faults == NULL) {
        return -ENOTSUP;
    }
    metalclaw_hal_lock();
    int err = inst->model->ops->clear_faults(inst);
    metalclaw_hal_unlock();
    return err;
}

static int check_region(const struct metalclaw_instance *inst, size_t region, uint32_t offset, size_t len, bool write) {
    if (inst == NULL || !inst->attached || region >= inst->model->region_count) {
        return -EINVAL;
    }
    const struct metalclaw_region_desc *desc = &inst->model->regions[region];
    if (offset > desc->size || len > desc->size - offset) {
        return -EINVAL;
    }
    if (write ? desc->access == METALCLAW_ACCESS_RO : desc->access == METALCLAW_ACCESS_WO) {
        return -ENOTSUP;
    }
    return 0;
}

int metalclaw_read_region(struct metalclaw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    int err = check_region(inst, region, offset, len, false);
    if (err != 0) {
        return err;
    }
    if (inst->model->ops->read_region == NULL) {
        return -ENOTSUP;
    }
    metalclaw_hal_lock();
    err = inst->model->ops->read_region(inst, region, offset, buf, len);
    metalclaw_hal_unlock();
    return err;
}

int metalclaw_write_region(struct metalclaw_instance *inst, size_t region, uint32_t offset, const void *buf, size_t len) {
    int err = check_region(inst, region, offset, len, true);
    if (err != 0) {
        return err;
    }
    if (inst->model->ops->write_region == NULL) {
        return -ENOTSUP;
    }
    metalclaw_hal_lock();
    err = inst->model->ops->write_region(inst, region, offset, buf, len);
    metalclaw_hal_unlock();
    return err;
}

int metalclaw_reset(struct metalclaw_instance *inst, bool factory) {
    if (inst == NULL || !inst->attached) {
        return -EINVAL;
    }
    if (inst->model->ops->reset == NULL) {
        return -ENOTSUP;
    }
    metalclaw_hal_lock();
    int err = inst->model->ops->reset(inst, factory);
    metalclaw_hal_unlock();
    return err;
}

int metalclaw_spi_mode_get(struct metalclaw_instance *inst, size_t binding, struct metalclaw_value *out) {
    if (binding >= inst->model->binding_count || inst->model->bindings[binding].kind != METALCLAW_BINDING_SPI_TARGET) {
        return -EINVAL;
    }
    if (!inst->bindings[binding].present) {
        return -ENODEV;
    }
    out->v[0] = metalclaw_hal_spi_target_mode(inst->bindings[binding].spi);
    return 0;
}

int metalclaw_spi_mode_set(struct metalclaw_instance *inst, size_t binding, const struct metalclaw_value *in) {
    if (binding >= inst->model->binding_count || inst->model->bindings[binding].kind != METALCLAW_BINDING_SPI_TARGET) {
        return -EINVAL;
    }
    uint8_t mode = (uint8_t)in->v[0];
    if (mode > 3 || (inst->model->bindings[binding].spi.modes & METALCLAW_SPI_MODE_BIT(mode)) == 0) {
        return -EINVAL;
    }
    if (!inst->bindings[binding].present) {
        return -ENODEV;
    }
    return metalclaw_hal_spi_target_set_mode(inst->bindings[binding].spi, mode);
}

int metalclaw_binding_stats(struct metalclaw_instance *inst, size_t binding, struct metalclaw_stat *out, size_t max) {
    if (inst == NULL || !inst->attached || out == NULL || binding >= inst->model->binding_count) {
        return -EINVAL;
    }
    const struct metalclaw_binding *b = &inst->bindings[binding];
    if (!b->present) {
        return -EINVAL;
    }
    size_t n = 0;
#define STAT(field_name, field_value) \
    do { \
        if (n < max) { \
            out[n].name = field_name; \
            out[n].value = field_value; \
        } \
        n++; \
    } while (0)
    switch (b->kind) {
        case METALCLAW_BINDING_I2C_TARGET: {
            struct metalclaw_i2c_target_stats st;
            metalclaw_hal_i2c_target_stats(b->i2c, &st);
            STAT("transactions", st.transactions);
            STAT("bytes", st.bytes);
            break;
        }
        case METALCLAW_BINDING_SPI_TARGET: {
            struct metalclaw_spi_target_stats st;
            metalclaw_hal_spi_target_stats(b->spi, &st);
            STAT("transactions", st.transactions);
            STAT("bytes", st.bytes);
            STAT("short_windows", st.short_windows);
            STAT("long_windows", st.long_windows);
            STAT("dma_faults", st.dma_faults);
            STAT("stale_windows", st.stale_windows);
            STAT("max_clock_hz", metalclaw_hal_spi_target_max_clock_hz(b->spi));
            break;
        }
        case METALCLAW_BINDING_UART: {
            struct metalclaw_uart_stats st;
            metalclaw_hal_uart_stats(b->uart, &st);
            STAT("bytes_rx", st.bytes_rx);
            STAT("bytes_tx", st.bytes_tx);
            STAT("overruns", st.overruns);
            STAT("frame_errors", st.frame_errors);
            STAT("baud", metalclaw_hal_uart_baud(b->uart));
            break;
        }
        default:
            return -ENOTSUP;
    }
#undef STAT
    return n > max ? (int)max : (int)n;
}

const char *metalclaw_value_type_name(enum metalclaw_value_type type) {
    switch (type) {
        case METALCLAW_TYPE_BOOL:
            return "bool";
        case METALCLAW_TYPE_I32:
            return "i32";
        case METALCLAW_TYPE_U32:
            return "u32";
    }
    return "?";
}

const char *metalclaw_access_name(enum metalclaw_access access) {
    switch (access) {
        case METALCLAW_ACCESS_RO:
            return "ro";
        case METALCLAW_ACCESS_WO:
            return "wo";
        case METALCLAW_ACCESS_RW:
            return "rw";
    }
    return "?";
}

const char *metalclaw_binding_kind_name(enum metalclaw_binding_kind kind) {
    switch (kind) {
        case METALCLAW_BINDING_GPIO_IN:
            return "gpio_in";
        case METALCLAW_BINDING_GPIO_OUT:
            return "gpio_out";
        case METALCLAW_BINDING_SPI_TARGET:
            return "spi_target";
        case METALCLAW_BINDING_I2C_TARGET:
            return "i2c_target";
        case METALCLAW_BINDING_UART:
            return "uart";
        case METALCLAW_BINDING_SWD_WIRE:
            return "swd_wire";
        case METALCLAW_BINDING_USB_INTERFACE:
            return "usb_interface";
    }
    return "?";
}

const char *metalclaw_uart_parity_name(enum metalclaw_uart_parity parity) {
    switch (parity) {
        case METALCLAW_UART_PARITY_NONE:
            return "none";
        case METALCLAW_UART_PARITY_EVEN:
            return "even";
        case METALCLAW_UART_PARITY_ODD:
            return "odd";
    }
    return "?";
}

const char *metalclaw_trace_kind_name(enum metalclaw_trace_kind kind) {
    switch (kind) {
        case METALCLAW_TRACE_SPI:
            return "spi";
        case METALCLAW_TRACE_I2C:
            return "i2c";
        case METALCLAW_TRACE_UART:
            return "uart";
        case METALCLAW_TRACE_COMMAND:
            return "command";
        case METALCLAW_TRACE_STATE:
            return "state";
        case METALCLAW_TRACE_ERROR:
            return "error";
    }
    return "?";
}

uint64_t metalclaw_now_us(void) {
    uint32_t state = metalclaw_hal_critical_enter();
    uint64_t now = clock_us;
    metalclaw_hal_critical_exit(state);
    return now;
}

static void advance(uint64_t delta_us) {
    uint32_t state = metalclaw_hal_critical_enter();
    clock_us += delta_us;
    uint64_t now = clock_us;
    metalclaw_hal_critical_exit(state);
    for (size_t i = 0; i < METALCLAW_INSTANCES_MAX; i++) {
        struct metalclaw_instance *inst = &instances[i];
        if (inst->attached && inst->model->ops->time_advanced != NULL) {
            inst->model->ops->time_advanced(inst, now);
        }
    }
}

static void clock_tick(void *ctx) {
    (void)ctx;
    uint64_t real = metalclaw_hal_time_us();

    if (clock_paused || real - clock_last_real_us < CLOCK_TICK_US || !metalclaw_hal_try_lock()) {
        return;
    }
    if (!clock_paused) {
        advance(real - clock_last_real_us);
        clock_last_real_us = real;
    }
    metalclaw_hal_unlock();
}

int metalclaw_advance_us(uint64_t delta_us) {
    metalclaw_hal_lock();
    if (!clock_paused) {
        metalclaw_hal_unlock();
        return -EBUSY;
    }
    advance(delta_us);
    metalclaw_hal_unlock();
    return 0;
}

void metalclaw_clock_pause(void) {
    metalclaw_hal_lock();
    clock_paused = true;
    metalclaw_hal_unlock();
}

void metalclaw_clock_resume(void) {
    metalclaw_hal_lock();
    clock_last_real_us = metalclaw_hal_time_us();
    clock_paused = false;
    metalclaw_hal_unlock();
}

bool metalclaw_clock_running(void) {
    return !clock_paused;
}

void metalclaw_runtime_init(void) {
    clock_last_real_us = metalclaw_hal_time_us();
    (void)metalclaw_hal_poll_register(clock_tick, NULL);
}

void metalclaw_trace(struct metalclaw_instance *inst, enum metalclaw_trace_kind kind,
    uint32_t addr, uint16_t len, int16_t result) {
    uint32_t state = metalclaw_hal_critical_enter();
    struct metalclaw_trace_entry *e = &inst->trace[inst->trace_head];
    e->t_us = clock_us;
    e->kind = (uint8_t)kind;
    e->addr = addr;
    e->len = len;
    e->result = result;
    inst->trace_head = (uint8_t)((inst->trace_head + 1u) % METALCLAW_TRACE_ENTRIES);
    if (inst->trace_count < METALCLAW_TRACE_ENTRIES) {
        inst->trace_count++;
    }
    metalclaw_hal_critical_exit(state);
}
