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

int mclw_check(int ret) {
    if (ret < 0) {
        mp_raise_OSError(-ret);
    }
    return ret;
}

static void dict_store(mp_obj_t dict, const char *key, mp_obj_t value) {
    mp_obj_dict_store(dict, mp_obj_new_str_from_cstr(key), value);
}

static void binding_facts(mp_obj_t d, const struct mclw_binding_desc *b) {
    if (b->kind == MCLW_BINDING_SPI_TARGET) {
        mp_obj_t modes[4];
        size_t n = 0;
        for (uint8_t m = 0; m < 4; m++) {
            if (b->spi.modes & MCLW_SPI_MODE_BIT(m)) {
                modes[n++] = MP_OBJ_NEW_SMALL_INT(m);
            }
        }
        dict_store(d, "modes", mp_obj_new_tuple(n, modes));
        dict_store(d, "default_mode", MP_OBJ_NEW_SMALL_INT(b->spi.default_mode));
        dict_store(d, "msb_first", mp_obj_new_bool(b->spi.msb_first));
    } else if (b->kind == MCLW_BINDING_UART) {
        dict_store(d, "default_baud", mp_obj_new_int_from_uint(b->uart.default_baud));
        dict_store(d, "data_bits", MP_OBJ_NEW_SMALL_INT(b->uart.format.data_bits));
        dict_store(d, "stop_bits", MP_OBJ_NEW_SMALL_INT(b->uart.format.stop_bits));
        dict_store(d, "parity", mp_obj_new_str_from_cstr(mclw_uart_parity_name(b->uart.format.parity)));
    }
}

mp_obj_t mclw_model_schema(const struct mclw_model *model) {
    mp_obj_t schema = mp_obj_new_dict(5);
    dict_store(schema, "id", mp_obj_new_str_from_cstr(model->id));
    dict_store(schema, "family", mp_obj_new_str_from_cstr(model->family));

    mp_obj_t properties = mp_obj_new_dict(model->property_count);
    for (size_t i = 0; i < model->property_count; i++) {
        const struct mclw_property_desc *p = &model->properties[i];
        mp_obj_t d = mp_obj_new_dict(6);
        dict_store(d, "type", mp_obj_new_str_from_cstr(mclw_value_type_name(p->type)));
        dict_store(d, "count", MP_OBJ_NEW_SMALL_INT(p->count));
        dict_store(d, "unit", p->unit ? mp_obj_new_str_from_cstr(p->unit) : mp_const_none);
        dict_store(d, "min", mp_obj_new_int_from_ll(p->min));
        dict_store(d, "max", mp_obj_new_int_from_ll(p->max));
        dict_store(d, "access", mp_obj_new_str_from_cstr(mclw_access_name(p->access)));
        dict_store(properties, p->name, d);
    }
    dict_store(schema, "properties", properties);

    mp_obj_t regions = mp_obj_new_dict(model->region_count);
    for (size_t i = 0; i < model->region_count; i++) {
        const struct mclw_region_desc *r = &model->regions[i];
        mp_obj_t d = mp_obj_new_dict(2);
        dict_store(d, "size", mp_obj_new_int_from_uint(r->size));
        dict_store(d, "access", mp_obj_new_str_from_cstr(mclw_access_name(r->access)));
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
        const struct mclw_binding_desc *b = &model->bindings[i];
        mp_obj_t d = mp_obj_new_dict(6);
        dict_store(d, "kind", mp_obj_new_str_from_cstr(mclw_binding_kind_name(b->kind)));
        dict_store(d, "required", mp_obj_new_bool(b->required));
        binding_facts(d, b);
        dict_store(bindings, b->name, d);
    }
    dict_store(schema, "bindings", bindings);
    return schema;
}

static mp_obj_t mclw_version(void) {
    return mp_obj_new_str_from_cstr(MCLW_VERSION_STRING);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mclw_version_obj, mclw_version);

static mp_obj_t mclw_models_fn(void) {
    mp_obj_t *items = m_new(mp_obj_t, mclw_model_count + 1);
    for (size_t i = 0; i < mclw_model_count; i++) {
        items[i] = mp_obj_new_str_from_cstr(mclw_models[i]->id);
    }
    return mp_obj_new_tuple(mclw_model_count, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mclw_models_obj, mclw_models_fn);

static const struct mclw_model *require_model(mp_obj_t id_in) {
    const struct mclw_model *model = mclw_model_find(mp_obj_str_get_str(id_in));
    if (model == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("no such model"));
    }
    return model;
}

static mp_obj_t mclw_schema(mp_obj_t id_in) {
    return mclw_model_schema(require_model(id_in));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mclw_schema_obj, mclw_schema);

static void pins_from_obj(mp_obj_t obj, struct mclw_binding_request *req) {
    size_t len = 1;
    mp_obj_t *items = &obj;
    if (!mp_obj_is_int(obj)) {
        mp_obj_get_array(obj, &len, &items);
    }
    if (len == 0 || len > MCLW_BINDING_PINS_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad pin count"));
    }
    for (size_t i = 0; i < len; i++) {
        mp_int_t pin = mp_obj_get_int(items[i]);
        if (pin < 0 || pin > 0xff) {
            mp_raise_ValueError(MP_ERROR_TEXT("bad pin"));
        }
        req->pins[i] = (uint8_t)pin;
    }
    req->pin_count = (uint8_t)len;
}

static mp_obj_t mclw_attach_fn(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    if (n_args != 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("attach(model, name, **bindings)"));
    }
    const struct mclw_model *model = require_model(pos_args[0]);
    const char *name = mp_obj_str_get_str(pos_args[1]);

    struct mclw_binding_request requests[MCLW_INSTANCE_BINDINGS_MAX];
    memset(requests, 0, sizeof(requests));
    if (model->binding_count > MCLW_INSTANCE_BINDINGS_MAX) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("model declares too many bindings"));
    }

    for (size_t i = 0; i < kw_args->alloc; i++) {
        if (!mp_map_slot_is_filled(kw_args, i)) {
            continue;
        }
        const char *key = mp_obj_str_get_str(kw_args->table[i].key);
        int index = mclw_binding_index(model, key);
        if (index < 0) {
            mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("unexpected keyword '%s'"), key);
        }
        const struct mclw_binding_desc *desc = &model->bindings[index];
        struct mclw_binding_request *req = &requests[index];
        req->present = true;
        switch (desc->kind) {
            case MCLW_BINDING_GPIO_IN:
            case MCLW_BINDING_GPIO_OUT:
            case MCLW_BINDING_SWD_WIRE:
            case MCLW_BINDING_I2C_TARGET:
            case MCLW_BINDING_SPI_TARGET:
            case MCLW_BINDING_UART:
                pins_from_obj(kw_args->table[i].value, req);
                break;
            case MCLW_BINDING_USB_INTERFACE:
                mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("'%s' is allocated by the harness"), key);
            default:
                mp_raise_NotImplementedError(MP_ERROR_TEXT("binding kind not available on this port"));
        }
    }
    for (size_t i = 0; i < model->binding_count; i++) {
        if (model->bindings[i].required && !requests[i].present
            && model->bindings[i].kind != MCLW_BINDING_USB_INTERFACE) {
            mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("missing required binding '%s'"),
                model->bindings[i].name);
        }
    }

    struct mclw_instance *inst = NULL;
    int err = mclw_attach(model, name, requests, &inst);
    if (err == -EEXIST) {
        mp_raise_ValueError(MP_ERROR_TEXT("name already attached"));
    }
    mclw_check(err);
    return mclw_instance_wrap(inst);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mclw_attach_obj, 2, mclw_attach_fn);

static mp_obj_t mclw_instances(void) {
    size_t count = mclw_instance_count();
    mp_obj_t *items = m_new(mp_obj_t, count + 1);
    for (size_t i = 0; i < count; i++) {
        items[i] = mclw_instance_wrap(mclw_instance_at(i));
    }
    return mp_obj_new_tuple(count, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(mclw_instances_obj, mclw_instances);

static mp_obj_t mclw_detach_fn(mp_obj_t name_in) {
    struct mclw_instance *inst = mclw_instance_find(mp_obj_str_get_str(name_in));
    if (inst == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("no such instance"));
    }
    mclw_check(mclw_detach(inst));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mclw_detach_obj, mclw_detach_fn);

static mp_obj_t mclw_now_us_fn(void) {
    return mp_obj_new_int_from_ull(mclw_now_us());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mclw_now_us_obj, mclw_now_us_fn);

static mp_obj_t mclw_advance_us_fn(mp_obj_t delta_in) {
    mp_int_t delta = mp_obj_get_int(delta_in);
    if (delta < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("delta must not be negative"));
    }
    if (mclw_advance_us((uint64_t)delta) == -EBUSY) {
        mp_raise_ValueError(MP_ERROR_TEXT("clock is running; pause() first"));
    }
    return mp_obj_new_int_from_ull(mclw_now_us());
}
static MP_DEFINE_CONST_FUN_OBJ_1(mclw_advance_us_obj, mclw_advance_us_fn);

static mp_obj_t mclw_pause_fn(void) {
    mclw_clock_pause();
    return mp_obj_new_int_from_ull(mclw_now_us());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mclw_pause_obj, mclw_pause_fn);

static mp_obj_t mclw_resume_fn(void) {
    mclw_clock_resume();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mclw_resume_obj, mclw_resume_fn);

static mp_obj_t mclw_running_fn(void) {
    return mp_obj_new_bool(mclw_clock_running());
}
static MP_DEFINE_CONST_FUN_OBJ_0(mclw_running_obj, mclw_running_fn);

static const mp_rom_map_elem_t mclw_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_metalclaw) },
    { MP_ROM_QSTR(MP_QSTR_version), MP_ROM_PTR(&mclw_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_models), MP_ROM_PTR(&mclw_models_obj) },
    { MP_ROM_QSTR(MP_QSTR_schema), MP_ROM_PTR(&mclw_schema_obj) },
    { MP_ROM_QSTR(MP_QSTR_attach), MP_ROM_PTR(&mclw_attach_obj) },
    { MP_ROM_QSTR(MP_QSTR_instances), MP_ROM_PTR(&mclw_instances_obj) },
    { MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&mclw_detach_obj) },
    { MP_ROM_QSTR(MP_QSTR_now_us), MP_ROM_PTR(&mclw_now_us_obj) },
    { MP_ROM_QSTR(MP_QSTR_advance_us), MP_ROM_PTR(&mclw_advance_us_obj) },
    { MP_ROM_QSTR(MP_QSTR_pause), MP_ROM_PTR(&mclw_pause_obj) },
    { MP_ROM_QSTR(MP_QSTR_resume), MP_ROM_PTR(&mclw_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_running), MP_ROM_PTR(&mclw_running_obj) },
    { MP_ROM_QSTR(MP_QSTR_Instance), MP_ROM_PTR(&mclw_instance_type) },
};
static MP_DEFINE_CONST_DICT(mclw_module_globals, mclw_module_globals_table);

const mp_obj_module_t mclw_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mclw_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_metalclaw, mclw_module);
