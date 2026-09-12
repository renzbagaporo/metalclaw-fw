// SPDX-License-Identifier: Apache-2.0 OR MIT
// The one module. Everything device-specific in it comes from the model
// descriptors at run time.

#include <errno.h>
#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"
#include "metalclaw/runtime.h"
#include "metalclaw/version.h"
#include "metalclaw_module.h"

int metalclaw_check(int ret) {
    if (ret < 0) {
        mp_raise_OSError(-ret);
    }
    return ret;
}

static const char *type_name(enum metalclaw_value_type type) {
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

static const char *access_name(enum metalclaw_access access) {
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

static const char *binding_kind_name(enum metalclaw_binding_kind kind) {
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
        case METALCLAW_BINDING_PWM_CAPTURE:
            return "pwm_capture";
        case METALCLAW_BINDING_SWD_WIRE:
            return "swd_wire";
        case METALCLAW_BINDING_USB_INTERFACE:
            return "usb_interface";
    }
    return "?";
}

static void dict_store(mp_obj_t dict, const char *key, mp_obj_t value) {
    mp_obj_dict_store(dict, mp_obj_new_str_from_cstr(key), value);
}

mp_obj_t metalclaw_model_schema(const struct metalclaw_model *model) {
    mp_obj_t schema = mp_obj_new_dict(5);
    dict_store(schema, "id", mp_obj_new_str_from_cstr(model->id));
    dict_store(schema, "family", mp_obj_new_str_from_cstr(model->family));

    mp_obj_t properties = mp_obj_new_dict(model->property_count);
    for (size_t i = 0; i < model->property_count; i++) {
        const struct metalclaw_property_desc *p = &model->properties[i];
        mp_obj_t d = mp_obj_new_dict(6);
        dict_store(d, "type", mp_obj_new_str_from_cstr(type_name(p->type)));
        dict_store(d, "count", MP_OBJ_NEW_SMALL_INT(p->count));
        dict_store(d, "unit", p->unit ? mp_obj_new_str_from_cstr(p->unit) : mp_const_none);
        dict_store(d, "min", mp_obj_new_int_from_ll(p->min));
        dict_store(d, "max", mp_obj_new_int_from_ll(p->max));
        dict_store(d, "access", mp_obj_new_str_from_cstr(access_name(p->access)));
        dict_store(properties, p->name, d);
    }
    dict_store(schema, "properties", properties);

    mp_obj_t regions = mp_obj_new_dict(model->region_count);
    for (size_t i = 0; i < model->region_count; i++) {
        const struct metalclaw_region_desc *r = &model->regions[i];
        mp_obj_t d = mp_obj_new_dict(2);
        dict_store(d, "size", mp_obj_new_int_from_uint(r->size));
        dict_store(d, "access", mp_obj_new_str_from_cstr(access_name(r->access)));
        dict_store(regions, r->name, d);
    }
    dict_store(schema, "regions", regions);

    mp_obj_t faults = mp_obj_new_tuple(0, NULL);
    if (model->fault_count > 0) {
        mp_obj_t *items = m_new(mp_obj_t, model->fault_count);
        for (size_t i = 0; i < model->fault_count; i++) {
            items[i] = mp_obj_new_str_from_cstr(model->faults[i]);
        }
        faults = mp_obj_new_tuple(model->fault_count, items);
    }
    dict_store(schema, "faults", faults);

    mp_obj_t bindings = mp_obj_new_dict(model->binding_count);
    for (size_t i = 0; i < model->binding_count; i++) {
        const struct metalclaw_binding_desc *b = &model->bindings[i];
        mp_obj_t d = mp_obj_new_dict(2);
        dict_store(d, "kind", mp_obj_new_str_from_cstr(binding_kind_name(b->kind)));
        dict_store(d, "required", mp_obj_new_bool(b->required));
        dict_store(bindings, b->name, d);
    }
    dict_store(schema, "bindings", bindings);
    return schema;
}

static mp_obj_t metalclaw_version(void) {
    return mp_obj_new_str_from_cstr(METALCLAW_VERSION_STRING);
}
static MP_DEFINE_CONST_FUN_OBJ_0(metalclaw_version_obj, metalclaw_version);

static mp_obj_t metalclaw_models_fn(void) {
    mp_obj_t *items = m_new(mp_obj_t, metalclaw_model_count + 1);
    for (size_t i = 0; i < metalclaw_model_count; i++) {
        items[i] = mp_obj_new_str_from_cstr(metalclaw_models[i]->id);
    }
    return mp_obj_new_tuple(metalclaw_model_count, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(metalclaw_models_obj, metalclaw_models_fn);

static const struct metalclaw_model *require_model(mp_obj_t id_in) {
    const struct metalclaw_model *model = metalclaw_model_find(mp_obj_str_get_str(id_in));
    if (model == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("no such model"));
    }
    return model;
}

static mp_obj_t metalclaw_schema(mp_obj_t id_in) {
    return metalclaw_model_schema(require_model(id_in));
}
static MP_DEFINE_CONST_FUN_OBJ_1(metalclaw_schema_obj, metalclaw_schema);

static mp_obj_t metalclaw_attach_fn(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    if (n_args != 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("attach(model, name, **bindings)"));
    }
    const struct metalclaw_model *model = require_model(pos_args[0]);
    const char *name = mp_obj_str_get_str(pos_args[1]);

    struct metalclaw_binding_request requests[METALCLAW_INSTANCE_BINDINGS_MAX];
    memset(requests, 0, sizeof(requests));
    if (model->binding_count > METALCLAW_INSTANCE_BINDINGS_MAX) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("model declares too many bindings"));
    }

    for (size_t i = 0; i < kw_args->alloc; i++) {
        if (!mp_map_slot_is_filled(kw_args, i)) {
            continue;
        }
        const char *key = mp_obj_str_get_str(kw_args->table[i].key);
        int index = metalclaw_binding_index(model, key);
        if (index < 0) {
            mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("unexpected keyword '%s'"), key);
        }
        const struct metalclaw_binding_desc *desc = &model->bindings[index];
        struct metalclaw_binding_request *req = &requests[index];
        req->present = true;
        switch (desc->kind) {
            case METALCLAW_BINDING_GPIO_IN:
            case METALCLAW_BINDING_GPIO_OUT:
                req->pin = mp_obj_get_int(kw_args->table[i].value);
                break;
            default:
                mp_raise_NotImplementedError(MP_ERROR_TEXT("binding kind not available on this port"));
        }
    }
    for (size_t i = 0; i < model->binding_count; i++) {
        if (model->bindings[i].required && !requests[i].present
            && model->bindings[i].kind != METALCLAW_BINDING_USB_INTERFACE) {
            mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("missing required binding '%s'"),
                model->bindings[i].name);
        }
    }

    struct metalclaw_instance *inst = NULL;
    int err = metalclaw_attach(model, name, requests, &inst);
    if (err == -EEXIST) {
        mp_raise_ValueError(MP_ERROR_TEXT("name already attached"));
    }
    metalclaw_check(err);
    return metalclaw_instance_wrap(inst);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(metalclaw_attach_obj, 2, metalclaw_attach_fn);

static mp_obj_t metalclaw_instances(void) {
    size_t count = metalclaw_instance_count();
    mp_obj_t *items = m_new(mp_obj_t, count + 1);
    for (size_t i = 0; i < count; i++) {
        items[i] = metalclaw_instance_wrap(metalclaw_instance_at(i));
    }
    return mp_obj_new_tuple(count, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(metalclaw_instances_obj, metalclaw_instances);

static mp_obj_t metalclaw_detach_fn(mp_obj_t name_in) {
    struct metalclaw_instance *inst = metalclaw_instance_find(mp_obj_str_get_str(name_in));
    if (inst == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("no such instance"));
    }
    metalclaw_check(metalclaw_detach(inst));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(metalclaw_detach_obj, metalclaw_detach_fn);

static mp_obj_t metalclaw_now_us_fn(void) {
    return mp_obj_new_int_from_ull(metalclaw_now_us());
}
static MP_DEFINE_CONST_FUN_OBJ_0(metalclaw_now_us_obj, metalclaw_now_us_fn);

static mp_obj_t metalclaw_advance_us_fn(mp_obj_t delta_in) {
    mp_int_t delta = mp_obj_get_int(delta_in);
    if (delta < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("delta must not be negative"));
    }
    metalclaw_advance_us((uint64_t)delta);
    return mp_obj_new_int_from_ull(metalclaw_now_us());
}
static MP_DEFINE_CONST_FUN_OBJ_1(metalclaw_advance_us_obj, metalclaw_advance_us_fn);

static const mp_rom_map_elem_t metalclaw_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_metalclaw) },
    { MP_ROM_QSTR(MP_QSTR_version), MP_ROM_PTR(&metalclaw_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_models), MP_ROM_PTR(&metalclaw_models_obj) },
    { MP_ROM_QSTR(MP_QSTR_schema), MP_ROM_PTR(&metalclaw_schema_obj) },
    { MP_ROM_QSTR(MP_QSTR_attach), MP_ROM_PTR(&metalclaw_attach_obj) },
    { MP_ROM_QSTR(MP_QSTR_instances), MP_ROM_PTR(&metalclaw_instances_obj) },
    { MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&metalclaw_detach_obj) },
    { MP_ROM_QSTR(MP_QSTR_now_us), MP_ROM_PTR(&metalclaw_now_us_obj) },
    { MP_ROM_QSTR(MP_QSTR_advance_us), MP_ROM_PTR(&metalclaw_advance_us_obj) },
    { MP_ROM_QSTR(MP_QSTR_Instance), MP_ROM_PTR(&metalclaw_instance_type) },
};
static MP_DEFINE_CONST_DICT(metalclaw_module_globals, metalclaw_module_globals_table);

const mp_obj_module_t metalclaw_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&metalclaw_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_metalclaw, metalclaw_module);
