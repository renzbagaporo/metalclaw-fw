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

static struct mclw_instance instances[METALCLAW_INSTANCES_MAX];
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

const struct mclw_model *mclw_model_find(const char *id) {
    for (size_t i = 0; i < mclw_model_count; i++) {
        if (strcmp(mclw_models[i]->id, id) == 0) {
            return mclw_models[i];
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

int mclw_property_index(const struct mclw_model *model, const char *name) {
    return index_of(&model->properties[0].name, sizeof(model->properties[0]), model->property_count, name);
}

int mclw_region_index(const struct mclw_model *model, const char *name) {
    return index_of(&model->regions[0].name, sizeof(model->regions[0]), model->region_count, name);
}

int mclw_fault_index(const struct mclw_model *model, const char *name) {
    return index_of(model->faults, sizeof(model->faults[0]), model->fault_count, name);
}

int mclw_binding_index(const struct mclw_model *model, const char *name) {
    return index_of(&model->bindings[0].name, sizeof(model->bindings[0]), model->binding_count, name);
}

struct mclw_instance *mclw_instance_find(const char *name) {
    for (size_t i = 0; i < METALCLAW_INSTANCES_MAX; i++) {
        if (instances[i].attached && strcmp(instances[i].name, name) == 0) {
            return &instances[i];
        }
    }
    return NULL;
}

struct mclw_instance *MCLW_RAM_FUNC(mclw_instance_from_state)(const void *state) {
    const uint8_t *p = state;

    if (p < arena || p >= arena + METALCLAW_ARENA_SIZE) {
        return NULL;
    }
    uint8_t tag = arena_owner[(size_t)(p - arena) / ARENA_PAGE];
    return tag == 0 ? NULL : &instances[tag - 1u];
}

size_t mclw_instance_count(void) {
    return attached_count;
}

struct mclw_instance *mclw_instance_at(size_t index) {
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

static void release_bindings(struct mclw_instance *inst) {
    for (size_t i = 0; i < MCLW_INSTANCE_BINDINGS_MAX; i++) {
        struct mclw_binding *b = &inst->bindings[i];
        if (!b->present) {
            continue;
        }
        switch (b->kind) {
            case MCLW_BINDING_GPIO_IN:
            case MCLW_BINDING_GPIO_OUT:
                mclw_hal_gpio_close(b->gpio);
                break;
            case MCLW_BINDING_SWD_WIRE:
                mclw_hal_swd_close(b->swd);
                break;
            case MCLW_BINDING_I2C_TARGET:
                mclw_hal_i2c_target_close(b->i2c);
                break;
            case MCLW_BINDING_SPI_TARGET:
                mclw_hal_spi_target_close(b->spi);
                break;
            case MCLW_BINDING_UART:
                mclw_hal_uart_close(b->uart);
                break;
            case MCLW_BINDING_USB_INTERFACE:
                mclw_hal_usb_close(b->usb);
                break;
            default:
                break;
        }
        b->present = false;
        b->handle = NULL;
    }
    mclw_hal_pin_release_all(inst);
}

static void release_arena(const struct mclw_instance *inst) {
    uint8_t tag = (uint8_t)(inst - instances) + 1u;

    for (size_t i = 0; i < ARENA_PAGES; i++) {
        if (arena_owner[i] == tag) {
            arena_owner[i] = 0;
        }
    }
}

struct usb_context {
    struct mclw_instance *inst;
    size_t binding;
};

// Bytes for an instance that is not attached, or has no usb_rx, are taken
// and dropped so the host is never held off by nobody.
static size_t usb_rx_trampoline(struct mclw_usb_interface *itf, const uint8_t *data, size_t len, void *ctx) {
    (void)itf;
    struct usb_context *c = ctx;
    size_t taken = len;
    mclw_hal_lock();
    if (c->inst->attached && c->inst->model->ops->usb_rx != NULL) {
        taken = c->inst->model->ops->usb_rx(c->inst, c->binding, data, len);
    }
    mclw_hal_unlock();
    return taken < len ? taken : len;
}

static void usb_control_trampoline(struct mclw_usb_interface *itf, void *ctx) {
    (void)itf;
    struct usb_context *c = ctx;
    mclw_hal_lock();
    if (c->inst->attached && c->inst->model->ops->usb_control != NULL) {
        c->inst->model->ops->usb_control(c->inst, c->binding);
    }
    mclw_hal_unlock();
}

static struct usb_context usb_contexts[METALCLAW_INSTANCES_MAX][MCLW_INSTANCE_BINDINGS_MAX];

static int resolve_binding(struct mclw_instance *inst, size_t index, const struct mclw_binding_desc *desc,
    const struct mclw_binding_request *req, struct mclw_binding *out) {
    int err = 0;
    out->kind = desc->kind;
    out->present = false;
    out->handle = NULL;

    if (desc->kind == MCLW_BINDING_USB_INTERFACE) {
        if (desc->usb == NULL) {
            return -EINVAL;
        }
        struct usb_context *ctx = &usb_contexts[inst - instances][index];
        ctx->inst = inst;
        ctx->binding = index;
        out->usb = mclw_hal_usb_open(desc->usb, usb_rx_trampoline, usb_control_trampoline, ctx, inst, &err);
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
        case MCLW_BINDING_GPIO_IN:
        case MCLW_BINDING_GPIO_OUT:
            if (req->pin_count != 1) {
                return -EINVAL;
            }
            out->gpio = mclw_hal_gpio_open(req->pins[0], desc->kind == MCLW_BINDING_GPIO_OUT,
                MCLW_GPIO_PULL_NONE, false, inst, &err);
            if (out->gpio == NULL) {
                return err;
            }
            out->present = true;
            return 0;
        case MCLW_BINDING_I2C_TARGET:
            if (req->pin_count != 3) {
                return -EINVAL;
            }
            out->i2c = mclw_hal_i2c_target_open(req->pins[0], req->pins[1], req->pins[2], inst, &err);
            if (out->i2c == NULL) {
                return err;
            }
            out->present = true;
            return 0;
        case MCLW_BINDING_SPI_TARGET:
            if (req->pin_count != 4) {
                return -EINVAL;
            }
            out->spi = mclw_hal_spi_target_open(req->pins[0], req->pins[1], req->pins[2], req->pins[3],
                desc->spi.default_mode, desc->spi.msb_first, inst, &err);
            if (out->spi == NULL) {
                return err;
            }
            out->present = true;
            return 0;
        case MCLW_BINDING_UART:
            if (req->pin_count != 2) {
                return -EINVAL;
            }
            out->uart = mclw_hal_uart_open(req->pins[0], req->pins[1], desc->uart.default_baud,
                &desc->uart.format, inst, &err);
            if (out->uart == NULL) {
                return err;
            }
            out->present = true;
            return 0;
        case MCLW_BINDING_SWD_WIRE:
            if (req->pin_count != 2 && req->pin_count != 3) {
                return -EINVAL;
            }
            out->swd = mclw_hal_swd_open(req->pins[0], req->pins[1],
                req->pin_count == 3 ? req->pins[2] : MCLW_PIN_NONE, inst, &err);
            if (out->swd == NULL) {
                return err;
            }
            out->present = true;
            return 0;
        default:
            return -ENOTSUP;
    }
}

int mclw_attach(const struct mclw_model *model, const char *name,
    const struct mclw_binding_request *requests, struct mclw_instance **out) {
    if (model == NULL || name == NULL || out == NULL) {
        return -EINVAL;
    }
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > MCLW_INSTANCE_NAME_MAX) {
        return -EINVAL;
    }
    if (model->abi_version != MCLW_ABI_VERSION || model->binding_count > MCLW_INSTANCE_BINDINGS_MAX) {
        return -ENOTSUP;
    }
    mclw_hal_lock();
    if (mclw_instance_find(name) != NULL) {
        mclw_hal_unlock();
        return -EEXIST;
    }

    struct mclw_instance *inst = NULL;
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
        mclw_hal_unlock();
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
        mclw_hal_unlock();
        return err;
    }

    uint32_t state = mclw_hal_critical_enter();
    inst->attached = true;
    attached_count++;
    mclw_hal_critical_exit(state);
    mclw_hal_unlock();
    *out = inst;
    return 0;
}

int mclw_detach(struct mclw_instance *inst) {
    if (inst == NULL) {
        return -ENOENT;
    }
    mclw_hal_lock();
    if (!inst->attached) {
        mclw_hal_unlock();
        return -ENOENT;
    }
    uint32_t state = mclw_hal_critical_enter();
    inst->attached = false;
    attached_count--;
    mclw_hal_critical_exit(state);

    if (inst->model->ops->deinit != NULL) {
        inst->model->ops->deinit(inst);
    }
    release_bindings(inst);
    release_arena(inst);
    inst->model = NULL;
    inst->state = NULL;
    mclw_hal_unlock();
    return 0;
}

void mclw_detach_all(void) {
    for (size_t i = 0; i < METALCLAW_INSTANCES_MAX; i++) {
        if (instances[i].attached) {
            mclw_detach(&instances[i]);
        }
    }
}

static int check_value(const struct mclw_property_desc *desc, const struct mclw_value *in) {
    if (in->count != desc->count) {
        return -EINVAL;
    }
    for (size_t i = 0; i < in->count; i++) {
        int64_t v = in->v[i];
        if (v < desc->min || v > desc->max) {
            return -EINVAL;
        }
        switch (desc->type) {
            case MCLW_TYPE_BOOL:
                if (v != 0 && v != 1) {
                    return -EINVAL;
                }
                break;
            case MCLW_TYPE_I32:
                if (v < INT32_MIN || v > INT32_MAX) {
                    return -EINVAL;
                }
                break;
            case MCLW_TYPE_U32:
                if (v < 0 || v > UINT32_MAX) {
                    return -EINVAL;
                }
                break;
        }
    }
    return 0;
}

int mclw_get(struct mclw_instance *inst, size_t property, struct mclw_value *out) {
    if (inst == NULL || !inst->attached || out == NULL || property >= inst->model->property_count) {
        return -EINVAL;
    }
    const struct mclw_property_desc *desc = &inst->model->properties[property];
    if (desc->access == MCLW_ACCESS_WO || inst->model->ops->get == NULL) {
        return -ENOTSUP;
    }
    memset(out, 0, sizeof(*out));
    out->count = desc->count;
    mclw_hal_lock();
    int err = inst->model->ops->get(inst, property, out);
    mclw_hal_unlock();
    return err;
}

int mclw_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in) {
    if (inst == NULL || !inst->attached || in == NULL || property >= inst->model->property_count) {
        return -EINVAL;
    }
    const struct mclw_property_desc *desc = &inst->model->properties[property];
    if (desc->access == MCLW_ACCESS_RO || inst->model->ops->set == NULL) {
        return -ENOTSUP;
    }
    int err = check_value(desc, in);
    if (err != 0) {
        return err;
    }
    mclw_hal_lock();
    err = inst->model->ops->set(inst, property, in);
    mclw_hal_unlock();
    return err;
}

int mclw_inject_fault(struct mclw_instance *inst, size_t fault, uint32_t count) {
    if (inst == NULL || !inst->attached || fault >= inst->model->fault_count) {
        return -EINVAL;
    }
    if (inst->model->ops->inject_fault == NULL) {
        return -ENOTSUP;
    }
    mclw_hal_lock();
    int err = inst->model->ops->inject_fault(inst, fault, count);
    mclw_hal_unlock();
    return err;
}

int mclw_clear_faults(struct mclw_instance *inst) {
    if (inst == NULL || !inst->attached) {
        return -EINVAL;
    }
    if (inst->model->ops->clear_faults == NULL) {
        return -ENOTSUP;
    }
    mclw_hal_lock();
    int err = inst->model->ops->clear_faults(inst);
    mclw_hal_unlock();
    return err;
}

static int check_region(const struct mclw_instance *inst, size_t region, uint32_t offset, size_t len, bool write) {
    if (inst == NULL || !inst->attached || region >= inst->model->region_count) {
        return -EINVAL;
    }
    const struct mclw_region_desc *desc = &inst->model->regions[region];
    if (offset > desc->size || len > desc->size - offset) {
        return -EINVAL;
    }
    if (write ? desc->access == MCLW_ACCESS_RO : desc->access == MCLW_ACCESS_WO) {
        return -ENOTSUP;
    }
    return 0;
}

int mclw_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len) {
    int err = check_region(inst, region, offset, len, false);
    if (err != 0) {
        return err;
    }
    if (inst->model->ops->read_region == NULL) {
        return -ENOTSUP;
    }
    mclw_hal_lock();
    err = inst->model->ops->read_region(inst, region, offset, buf, len);
    mclw_hal_unlock();
    return err;
}

int mclw_write_region(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf, size_t len) {
    int err = check_region(inst, region, offset, len, true);
    if (err != 0) {
        return err;
    }
    if (inst->model->ops->write_region == NULL) {
        return -ENOTSUP;
    }
    mclw_hal_lock();
    err = inst->model->ops->write_region(inst, region, offset, buf, len);
    mclw_hal_unlock();
    return err;
}

int mclw_reset(struct mclw_instance *inst, bool factory) {
    if (inst == NULL || !inst->attached) {
        return -EINVAL;
    }
    if (inst->model->ops->reset == NULL) {
        return -ENOTSUP;
    }
    mclw_hal_lock();
    int err = inst->model->ops->reset(inst, factory);
    mclw_hal_unlock();
    return err;
}

int mclw_spi_mode_get(struct mclw_instance *inst, size_t binding, struct mclw_value *out) {
    if (binding >= inst->model->binding_count || inst->model->bindings[binding].kind != MCLW_BINDING_SPI_TARGET) {
        return -EINVAL;
    }
    if (!inst->bindings[binding].present) {
        return -ENODEV;
    }
    out->v[0] = mclw_hal_spi_target_mode(inst->bindings[binding].spi);
    return 0;
}

int mclw_spi_mode_set(struct mclw_instance *inst, size_t binding, const struct mclw_value *in) {
    if (binding >= inst->model->binding_count || inst->model->bindings[binding].kind != MCLW_BINDING_SPI_TARGET) {
        return -EINVAL;
    }
    uint8_t mode = (uint8_t)in->v[0];
    if (mode > 3 || (inst->model->bindings[binding].spi.modes & MCLW_SPI_MODE_BIT(mode)) == 0) {
        return -EINVAL;
    }
    if (!inst->bindings[binding].present) {
        return -ENODEV;
    }
    return mclw_hal_spi_target_set_mode(inst->bindings[binding].spi, mode);
}

int mclw_binding_stats(struct mclw_instance *inst, size_t binding, struct mclw_stat *out, size_t max) {
    if (inst == NULL || !inst->attached || out == NULL || binding >= inst->model->binding_count) {
        return -EINVAL;
    }
    const struct mclw_binding *b = &inst->bindings[binding];
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
        case MCLW_BINDING_I2C_TARGET: {
            struct mclw_i2c_target_stats st;
            mclw_hal_i2c_target_stats(b->i2c, &st);
            STAT("transactions", st.transactions);
            STAT("bytes", st.bytes);
            break;
        }
        case MCLW_BINDING_SPI_TARGET: {
            struct mclw_spi_target_stats st;
            mclw_hal_spi_target_stats(b->spi, &st);
            STAT("transactions", st.transactions);
            STAT("bytes", st.bytes);
            STAT("short_windows", st.short_windows);
            STAT("long_windows", st.long_windows);
            STAT("dma_faults", st.dma_faults);
            STAT("stale_windows", st.stale_windows);
            STAT("max_clock_hz", mclw_hal_spi_target_max_clock_hz(b->spi));
            break;
        }
        case MCLW_BINDING_UART: {
            struct mclw_uart_stats st;
            mclw_hal_uart_stats(b->uart, &st);
            STAT("bytes_rx", st.bytes_rx);
            STAT("bytes_tx", st.bytes_tx);
            STAT("overruns", st.overruns);
            STAT("frame_errors", st.frame_errors);
            STAT("baud", mclw_hal_uart_baud(b->uart));
            break;
        }
        default:
            return -ENOTSUP;
    }
#undef STAT
    return n > max ? (int)max : (int)n;
}

const char *mclw_value_type_name(enum mclw_value_type type) {
    switch (type) {
        case MCLW_TYPE_BOOL:
            return "bool";
        case MCLW_TYPE_I32:
            return "i32";
        case MCLW_TYPE_U32:
            return "u32";
    }
    return "?";
}

const char *mclw_access_name(enum mclw_access access) {
    switch (access) {
        case MCLW_ACCESS_RO:
            return "ro";
        case MCLW_ACCESS_WO:
            return "wo";
        case MCLW_ACCESS_RW:
            return "rw";
    }
    return "?";
}

const char *mclw_binding_kind_name(enum mclw_binding_kind kind) {
    switch (kind) {
        case MCLW_BINDING_GPIO_IN:
            return "gpio_in";
        case MCLW_BINDING_GPIO_OUT:
            return "gpio_out";
        case MCLW_BINDING_SPI_TARGET:
            return "spi_target";
        case MCLW_BINDING_I2C_TARGET:
            return "i2c_target";
        case MCLW_BINDING_UART:
            return "uart";
        case MCLW_BINDING_SWD_WIRE:
            return "swd_wire";
        case MCLW_BINDING_USB_INTERFACE:
            return "usb_interface";
    }
    return "?";
}

const char *mclw_uart_parity_name(enum mclw_uart_parity parity) {
    switch (parity) {
        case MCLW_UART_PARITY_NONE:
            return "none";
        case MCLW_UART_PARITY_EVEN:
            return "even";
        case MCLW_UART_PARITY_ODD:
            return "odd";
    }
    return "?";
}

const char *mclw_trace_kind_name(enum mclw_trace_kind kind) {
    switch (kind) {
        case MCLW_TRACE_SPI:
            return "spi";
        case MCLW_TRACE_I2C:
            return "i2c";
        case MCLW_TRACE_UART:
            return "uart";
        case MCLW_TRACE_COMMAND:
            return "command";
        case MCLW_TRACE_STATE:
            return "state";
        case MCLW_TRACE_ERROR:
            return "error";
    }
    return "?";
}

// Between ticks a running clock reads as the last tick plus the real time
// since, so a model that stamps an event from a callback is not up to a
// tick (or a busy runtime lock's worth) behind.
uint64_t mclw_now_us(void) {
    uint32_t state = mclw_hal_critical_enter();
    uint64_t now = clock_us;
    if (!clock_paused) {
        now += mclw_hal_time_us() - clock_last_real_us;
    }
    mclw_hal_critical_exit(state);
    return now;
}

static void advance(uint64_t delta_us, uint64_t real_us) {
    uint32_t state = mclw_hal_critical_enter();
    clock_us += delta_us;
    if (!clock_paused) {
        clock_last_real_us = real_us;
    }
    uint64_t now = clock_us;
    mclw_hal_critical_exit(state);
    for (size_t i = 0; i < METALCLAW_INSTANCES_MAX; i++) {
        struct mclw_instance *inst = &instances[i];
        if (inst->attached && inst->model->ops->time_advanced != NULL) {
            inst->model->ops->time_advanced(inst, now);
        }
    }
}

static void clock_tick(void *ctx) {
    (void)ctx;
    uint64_t real = mclw_hal_time_us();

    if (clock_paused || real - clock_last_real_us < CLOCK_TICK_US || !mclw_hal_try_lock()) {
        return;
    }
    if (!clock_paused) {
        advance(real - clock_last_real_us, real);
    }
    mclw_hal_unlock();
}

int mclw_advance_us(uint64_t delta_us) {
    mclw_hal_lock();
    if (!clock_paused) {
        mclw_hal_unlock();
        return -EBUSY;
    }
    advance(delta_us, 0);
    mclw_hal_unlock();
    return 0;
}

void mclw_clock_pause(void) {
    mclw_hal_lock();
    uint32_t state = mclw_hal_critical_enter();
    if (!clock_paused) {
        clock_us += mclw_hal_time_us() - clock_last_real_us;
        clock_paused = true;
    }
    mclw_hal_critical_exit(state);
    mclw_hal_unlock();
}

void mclw_clock_resume(void) {
    mclw_hal_lock();
    uint32_t state = mclw_hal_critical_enter();
    if (clock_paused) {
        clock_last_real_us = mclw_hal_time_us();
        clock_paused = false;
    }
    mclw_hal_critical_exit(state);
    mclw_hal_unlock();
}

bool mclw_clock_running(void) {
    return !clock_paused;
}

void mclw_runtime_init(void) {
    clock_last_real_us = mclw_hal_time_us();
    (void)mclw_hal_poll_register(clock_tick, NULL);
}

void MCLW_RAM_FUNC(mclw_trace)(struct mclw_instance *inst, enum mclw_trace_kind kind,
    uint32_t addr, uint16_t len, int16_t result) {
    uint32_t state = mclw_hal_critical_enter();
    struct mclw_trace_entry *e = &inst->trace[inst->trace_head];
    e->t_us = clock_paused ? clock_us : clock_us + mclw_hal_time_us() - clock_last_real_us;
    e->kind = (uint8_t)kind;
    e->addr = addr;
    e->len = len;
    e->result = result;
    inst->trace_head = (uint8_t)((inst->trace_head + 1u) % MCLW_TRACE_ENTRIES);
    if (inst->trace_count < MCLW_TRACE_ENTRIES) {
        inst->trace_count++;
    }
    mclw_hal_critical_exit(state);
}
