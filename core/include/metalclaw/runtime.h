// SPDX-License-Identifier: Apache-2.0 OR MIT
// Instances, the clock, and everything a binding or a host needs to drive a
// device. Nothing here knows what any device is.

#ifndef METALCLAW_RUNTIME_H
#define METALCLAW_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/model.h"

#define METALCLAW_INSTANCE_NAME_MAX 31
#define METALCLAW_INSTANCE_BINDINGS_MAX 8
#define METALCLAW_TRACE_ENTRIES 16

struct metalclaw_trace_entry {
    uint64_t t_us;
    uint32_t addr;
    uint16_t len;
    int16_t result;
    uint8_t kind;
};

struct metalclaw_instance {
    const struct metalclaw_model *model;
    char name[METALCLAW_INSTANCE_NAME_MAX + 1];
    void *state;
    struct metalclaw_binding bindings[METALCLAW_INSTANCE_BINDINGS_MAX];
    uint32_t revision;
    // Bumped on every attach of this slot, so a stale reference to a detached
    // instance can be recognised.
    uint32_t generation;
    struct metalclaw_trace_entry trace[METALCLAW_TRACE_ENTRIES];
    uint8_t trace_head;
    uint8_t trace_count;
    bool attached;
};

// Registry, generated at build time from devices/*/device.cmake.
extern const struct metalclaw_model *const metalclaw_models[];
extern const size_t metalclaw_model_count;
const struct metalclaw_model *metalclaw_model_find(const char *id);

#define METALCLAW_BINDING_PINS_MAX 4

// A binding request as the user supplied it: one pin for gpio kinds, a pin
// tuple for swd_wire (swclk, swdio[, nreset]), i2c_target (sda, scl,
// address), spi_target (sck, mosi, miso, cs) and uart (tx, rx), nothing for
// usb_interface. `present` is false for an omitted optional.
struct metalclaw_binding_request {
    bool present;
    uint8_t pin_count;
    uint8_t pins[METALCLAW_BINDING_PINS_MAX];
    void *handle;
};

// Errors are negative errno values: -EINVAL bad name/index/value, -ENOENT no
// such model or instance, -EEXIST name taken, -ENOSPC arena or table full,
// -ENOTSUP the model or the platform lacks it, -EBUSY resource owned.
int metalclaw_attach(const struct metalclaw_model *model, const char *name,
    const struct metalclaw_binding_request *requests, struct metalclaw_instance **out);
int metalclaw_detach(struct metalclaw_instance *inst);
void metalclaw_detach_all(void);
struct metalclaw_instance *metalclaw_instance_find(const char *name);
size_t metalclaw_instance_count(void);
struct metalclaw_instance *metalclaw_instance_at(size_t index);

int metalclaw_property_index(const struct metalclaw_model *model, const char *name);
int metalclaw_region_index(const struct metalclaw_model *model, const char *name);
int metalclaw_fault_index(const struct metalclaw_model *model, const char *name);
int metalclaw_binding_index(const struct metalclaw_model *model, const char *name);

int metalclaw_get(struct metalclaw_instance *inst, size_t property, struct metalclaw_value *out);
int metalclaw_set(struct metalclaw_instance *inst, size_t property, const struct metalclaw_value *in);
int metalclaw_inject_fault(struct metalclaw_instance *inst, size_t fault, uint32_t count);
int metalclaw_clear_faults(struct metalclaw_instance *inst);
int metalclaw_read_region(struct metalclaw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len);
int metalclaw_write_region(struct metalclaw_instance *inst, size_t region, uint32_t offset, const void *buf, size_t len);
int metalclaw_reset(struct metalclaw_instance *inst, bool factory);

// Simulation time. It starts at zero and moves only when advanced.
uint64_t metalclaw_now_us(void);
void metalclaw_advance_us(uint64_t delta_us);

// For models: record what happened. Safe from interrupt context.
void metalclaw_trace(struct metalclaw_instance *inst, enum metalclaw_trace_kind kind,
    uint32_t addr, uint16_t len, int16_t result);

#endif
