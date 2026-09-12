// SPDX-License-Identifier: Apache-2.0 OR MIT
// What a device is, from the runtime's side. A device package defines one
// `struct metalclaw_model` and the runtime does everything else.

#ifndef METALCLAW_MODEL_H
#define METALCLAW_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/value.h"
#include "metalclaw/version.h"

enum metalclaw_access {
    METALCLAW_ACCESS_RO,
    METALCLAW_ACCESS_WO,
    METALCLAW_ACCESS_RW,
};

struct metalclaw_property_desc {
    const char *name;
    enum metalclaw_value_type type;
    uint8_t count;
    enum metalclaw_access access;
    const char *unit;
    int64_t min;
    int64_t max;
};

struct metalclaw_region_desc {
    const char *name;
    uint32_t size;
    enum metalclaw_access access;
};

// What a device needs from the platform. The name is the attach() keyword
// the user passes; the kind says what the runtime resolves it into.
// usb_interface entries have no keyword: the runtime allocates them.
enum metalclaw_binding_kind {
    METALCLAW_BINDING_GPIO_IN,
    METALCLAW_BINDING_GPIO_OUT,
    METALCLAW_BINDING_SPI_TARGET,
    METALCLAW_BINDING_I2C_TARGET,
    METALCLAW_BINDING_UART,
    METALCLAW_BINDING_PWM_CAPTURE,
    METALCLAW_BINDING_SWD_WIRE,
    METALCLAW_BINDING_USB_INTERFACE,
};

struct metalclaw_binding_desc {
    const char *name;
    enum metalclaw_binding_kind kind;
    bool required;
};

// Resolved binding handed to the model at init. Handles are opaque HAL
// objects; which member is valid follows from the kind.
struct metalclaw_binding {
    enum metalclaw_binding_kind kind;
    bool present;
    union {
        struct metalclaw_gpio *gpio;
        void *handle;
    };
};

struct metalclaw_instance;

// The only response image defined so far: a register file the SPI and I2C
// target engines can serve without calling back into the model.
struct metalclaw_regmap;

enum metalclaw_trace_kind {
    METALCLAW_TRACE_SPI,
    METALCLAW_TRACE_I2C,
    METALCLAW_TRACE_UART,
    METALCLAW_TRACE_GPIO,
    METALCLAW_TRACE_COMMAND,
    METALCLAW_TRACE_STATE,
    METALCLAW_TRACE_ERROR,
};

struct metalclaw_model_ops {
    // Required. `state` is zeroed, `state_size` bytes, from the runtime arena.
    int (*init)(struct metalclaw_instance *inst, const struct metalclaw_binding *bindings);
    void (*deinit)(struct metalclaw_instance *inst);

    // Optional from here on. A NULL op means the operation is unsupported.
    int (*reset)(struct metalclaw_instance *inst, bool factory);
    int (*get)(struct metalclaw_instance *inst, size_t property, struct metalclaw_value *out);
    int (*set)(struct metalclaw_instance *inst, size_t property, const struct metalclaw_value *in);
    int (*inject_fault)(struct metalclaw_instance *inst, size_t fault, uint32_t count);
    int (*clear_faults)(struct metalclaw_instance *inst);
    int (*read_region)(struct metalclaw_instance *inst, size_t region, uint32_t offset, void *buf, size_t len);
    int (*write_region)(struct metalclaw_instance *inst, size_t region, uint32_t offset, const void *buf, size_t len);
    // Render current state into the image the bound engine serves.
    int (*prepare)(struct metalclaw_instance *inst, struct metalclaw_regmap *image);
    void (*time_advanced)(struct metalclaw_instance *inst, uint64_t now_us);
};

struct metalclaw_model {
    uint32_t abi_version;
    const char *id;
    const char *family;
    const struct metalclaw_property_desc *properties;
    size_t property_count;
    const struct metalclaw_region_desc *regions;
    size_t region_count;
    const char *const *faults;
    size_t fault_count;
    const struct metalclaw_binding_desc *bindings;
    size_t binding_count;
    size_t state_size;
    const struct metalclaw_model_ops *ops;
};

#define METALCLAW_MODEL_CHECK(model_symbol) \
    _Static_assert((model_symbol).abi_version == METALCLAW_ABI_VERSION, \
    #model_symbol " targets a different MetalClaw ABI version")

#endif
