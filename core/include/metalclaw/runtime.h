// SPDX-License-Identifier: Apache-2.0 OR MIT
// Instances, the clock, and everything a binding or a host needs to drive a
// device. Nothing here knows what any device is.

#ifndef MCLW_RUNTIME_H
#define MCLW_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/model.h"

#define MCLW_INSTANCE_NAME_MAX 31
#define MCLW_INSTANCE_BINDINGS_MAX 8
#define MCLW_TRACE_ENTRIES 16

struct mclw_trace_entry {
    uint64_t t_us;
    uint32_t addr;
    uint16_t len;
    int16_t result;
    uint8_t kind;
};

struct mclw_instance {
    const struct mclw_model *model;
    char name[MCLW_INSTANCE_NAME_MAX + 1];
    void *state;
    struct mclw_binding bindings[MCLW_INSTANCE_BINDINGS_MAX];
    // Bumped on every attach of this slot, so a stale reference to a detached
    // instance can be recognised.
    uint32_t generation;
    struct mclw_trace_entry trace[MCLW_TRACE_ENTRIES];
    uint8_t trace_head;
    uint8_t trace_count;
    bool attached;
};

// Called once by the port before any script runs: starts the clock.
void mclw_runtime_init(void);

// Registry, generated at build time from devices/*/device.cmake.
extern const struct mclw_model *const mclw_models[];
extern const size_t mclw_model_count;
const struct mclw_model *mclw_model_find(const char *id);

#define MCLW_BINDING_PINS_MAX 4

// A binding request as the user supplied it: one pin for gpio kinds, a pin
// tuple for swd_wire (swclk, swdio[, nreset]), i2c_target (sda, scl,
// address), spi_target (sck, mosi, miso, cs) and uart (tx, rx), nothing for
// usb_interface. `present` is false for an omitted optional.
struct mclw_binding_request {
    bool present;
    uint8_t pin_count;
    uint8_t pins[MCLW_BINDING_PINS_MAX];
    void *handle;
};

// Errors are negative errno values: -EINVAL bad name/index/value, -ENOENT no
// such model or instance, -EEXIST name taken, -ENOSPC arena or table full,
// -ENOTSUP the model or the platform lacks it, -EBUSY resource owned.
int mclw_attach(const struct mclw_model *model, const char *name,
    const struct mclw_binding_request *requests, struct mclw_instance **out);
int mclw_detach(struct mclw_instance *inst);
void mclw_detach_all(void);
struct mclw_instance *mclw_instance_find(const char *name);
// The instance whose arena state `state` points into, for a callback that
// only holds its state. NULL for a pointer outside the arena.
struct mclw_instance *mclw_instance_from_state(const void *state);
size_t mclw_instance_count(void);
struct mclw_instance *mclw_instance_at(size_t index);

int mclw_property_index(const struct mclw_model *model, const char *name);
int mclw_region_index(const struct mclw_model *model, const char *name);
int mclw_fault_index(const struct mclw_model *model, const char *name);
int mclw_binding_index(const struct mclw_model *model, const char *name);

int mclw_get(struct mclw_instance *inst, size_t property, struct mclw_value *out);
int mclw_set(struct mclw_instance *inst, size_t property, const struct mclw_value *in);
int mclw_inject_fault(struct mclw_instance *inst, size_t fault, uint32_t count);
int mclw_clear_faults(struct mclw_instance *inst);
int mclw_read_region(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len);
int mclw_write_region(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf, size_t len);
int mclw_reset(struct mclw_instance *inst, bool factory);

// The SPI mode an spi_target binding listens in, for a model that exposes
// it as a property: the descriptor, and the get and set that validate
// against the binding's declared modes. -ENODEV when the binding is unbound.
#define MCLW_PROPERTY_SPI_MODE \
    { .name = "spi_mode", .type = MCLW_TYPE_U32, .count = 1, .access = MCLW_ACCESS_RW, \
      .unit = NULL, .min = 0, .max = 3 }
int mclw_spi_mode_get(struct mclw_instance *inst, size_t binding, struct mclw_value *out);
int mclw_spi_mode_set(struct mclw_instance *inst, size_t binding, const struct mclw_value *in);

// The bound engine's counters for one binding, as named values, so a
// binding or a host codec never switches on the kind. Returns how many were
// written, -EINVAL for an unbound binding, -ENOTSUP for a kind with none.
#define MCLW_STATS_MAX 8

struct mclw_stat {
    const char *name;
    uint32_t value;
};

int mclw_binding_stats(struct mclw_instance *inst, size_t binding, struct mclw_stat *out, size_t max);

// The names every schema and trace consumer uses.
const char *mclw_value_type_name(enum mclw_value_type type);
const char *mclw_access_name(enum mclw_access access);
const char *mclw_binding_kind_name(enum mclw_binding_kind kind);
const char *mclw_uart_parity_name(enum mclw_uart_parity parity);
const char *mclw_trace_kind_name(enum mclw_trace_kind kind);

// Simulation time. It starts at zero at boot and runs with real time on
// the worker, which calls every model's time_advanced as it moves. Paused,
// it moves only by advance_us(), which is -EBUSY while running.
uint64_t mclw_now_us(void);
int mclw_advance_us(uint64_t delta_us);
void mclw_clock_pause(void);
void mclw_clock_resume(void);
bool mclw_clock_running(void);

// For models: record what happened. Safe from interrupt context.
void mclw_trace(struct mclw_instance *inst, enum mclw_trace_kind kind,
    uint32_t addr, uint16_t len, int16_t result);

#endif
