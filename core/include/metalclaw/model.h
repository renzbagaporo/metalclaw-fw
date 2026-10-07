// SPDX-License-Identifier: Apache-2.0 OR MIT
// What a device is, from the runtime's side. A device package defines one
// `struct mclw_model` and the runtime does everything else.

#ifndef MCLW_MODEL_H
#define MCLW_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/bus.h"
#include "metalclaw/value.h"
#include "metalclaw/version.h"

enum mclw_access {
    MCLW_ACCESS_RO,
    MCLW_ACCESS_WO,
    MCLW_ACCESS_RW,
};

struct mclw_property_desc {
    const char *name;
    enum mclw_value_type type;
    uint8_t count;
    enum mclw_access access;
    const char *unit;
    int64_t min;
    int64_t max;
};

struct mclw_region_desc {
    const char *name;
    uint32_t size;
    enum mclw_access access;
};

// What a device needs from the platform. The name is the attach() keyword
// the user passes; the kind says what the runtime resolves it into.
// usb_interface entries have no keyword: the runtime allocates them.
// spi_target and uart carry the part's datasheet facts; the runtime opens
// the engine with them and the schema shows them to the fixture side.
enum mclw_binding_kind {
    MCLW_BINDING_GPIO_IN,
    MCLW_BINDING_GPIO_OUT,
    MCLW_BINDING_SPI_TARGET,
    MCLW_BINDING_I2C_TARGET,
    MCLW_BINDING_UART,
    MCLW_BINDING_SWD_WIRE,
    MCLW_BINDING_USB_INTERFACE,
};

struct mclw_usb_interface_desc;

struct mclw_binding_desc {
    const char *name;
    enum mclw_binding_kind kind;
    bool required;
    // Only for usb_interface: what the interface looks like to the host.
    const struct mclw_usb_interface_desc *usb;
    struct mclw_spi_facts spi;
    struct mclw_uart_facts uart;
};

// Resolved binding handed to the model at init. Handles are opaque HAL
// objects; which member is valid follows from the kind.
struct mclw_binding {
    enum mclw_binding_kind kind;
    bool present;
    union {
        struct mclw_gpio *gpio;
        struct mclw_swd *swd;
        struct mclw_i2c_target *i2c;
        struct mclw_spi_target *spi;
        struct mclw_uart *uart;
        struct mclw_usb_interface *usb;
        void *handle;
    };
};

struct mclw_instance;

enum mclw_trace_kind {
    MCLW_TRACE_SPI,
    MCLW_TRACE_I2C,
    MCLW_TRACE_UART,
    MCLW_TRACE_COMMAND,
    MCLW_TRACE_STATE,
    MCLW_TRACE_ERROR,
};

struct mclw_model_ops {
    // Required. `state` is zeroed, `state_size` bytes, from the runtime arena.
    int (*init)(struct mclw_instance *inst, const struct mclw_binding *bindings);
    void (*deinit)(struct mclw_instance *inst);

    // Optional from here on. A NULL op means the operation is unsupported.
    int (*reset)(struct mclw_instance *inst, bool factory);
    int (*get)(struct mclw_instance *inst, size_t property, struct mclw_value *out);
    int (*set)(struct mclw_instance *inst, size_t property, const struct mclw_value *in);
    int (*inject_fault)(struct mclw_instance *inst, size_t fault, uint32_t count);
    int (*clear_faults)(struct mclw_instance *inst);
    int (*read_region)(struct mclw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len);
    int (*write_region)(struct mclw_instance *inst, size_t region, uint32_t offset, const void *buf, size_t len);
    // The clock moved. Runs on the worker under the runtime lock, as often
    // as once a millisecond while the clock runs.
    void (*time_advanced)(struct mclw_instance *inst, uint64_t now_us);
    // A packet arrived on the usb_interface binding `binding`. Runs in the
    // USB task context under the runtime lock; answer through
    // mclw_hal_usb_write before returning.
    void (*usb_rx)(struct mclw_instance *inst, size_t binding, const uint8_t *data, size_t len);
};

struct mclw_model {
    uint32_t abi_version;
    const char *id;
    const char *family;
    // The model's own constant data, for one source that defines several
    // models. The runtime never reads it.
    const void *config;
    const struct mclw_property_desc *properties;
    size_t property_count;
    const struct mclw_region_desc *regions;
    size_t region_count;
    const char *const *faults;
    size_t fault_count;
    const struct mclw_binding_desc *bindings;
    size_t binding_count;
    size_t state_size;
    const struct mclw_model_ops *ops;
};

#endif
