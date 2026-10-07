// SPDX-License-Identifier: Apache-2.0 OR MIT
// One Python type for every device. Property names become attributes; the
// rest is the runtime API by another name.

#include <errno.h>
#include <string.h>

#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "metalclaw/runtime.h"
#include "metalclaw_module.h"

typedef struct _instance_obj_t {
    mp_obj_base_t base;
    struct mclw_instance *inst;
    uint32_t generation;
} instance_obj_t;

mp_obj_t mclw_instance_wrap(struct mclw_instance *inst) {
    instance_obj_t *o = mp_obj_malloc(instance_obj_t, &mclw_instance_type);
    o->inst = inst;
    o->generation = inst->generation;
    return MP_OBJ_FROM_PTR(o);
}

struct mclw_instance *mclw_instance_unwrap(mp_obj_t obj) {
    instance_obj_t *o = MP_OBJ_TO_PTR(obj);
    if (!o->inst->attached || o->inst->generation != o->generation) {
        mp_raise_ValueError(MP_ERROR_TEXT("instance is detached"));
    }
    return o->inst;
}

static int require_index(int index, const char *what) {
    if (index < 0) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("no such %s"), what);
    }
    return index;
}

static mp_obj_t value_to_obj(const struct mclw_property_desc *desc, const struct mclw_value *v) {
    mp_obj_t items[MCLW_VALUE_MAX_COUNT];
    for (size_t i = 0; i < v->count; i++) {
        items[i] = desc->type == MCLW_TYPE_BOOL ? mp_obj_new_bool(v->v[i] != 0) : mp_obj_new_int_from_ll(v->v[i]);
    }
    return v->count == 1 ? items[0] : mp_obj_new_tuple(v->count, items);
}

static void obj_to_value(const struct mclw_property_desc *desc, mp_obj_t obj, struct mclw_value *v) {
    memset(v, 0, sizeof(*v));
    if (desc->count == 1) {
        v->count = 1;
        v->v[0] = mp_obj_is_bool(obj) ? (mp_obj_is_true(obj) ? 1 : 0) : mp_obj_get_ll(obj);
        return;
    }
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(obj, &len, &items);
    if (len != desc->count) {
        mp_raise_ValueError(MP_ERROR_TEXT("wrong number of elements"));
    }
    v->count = (uint8_t)len;
    for (size_t i = 0; i < len; i++) {
        v->v[i] = mp_obj_is_bool(items[i]) ? (mp_obj_is_true(items[i]) ? 1 : 0) : mp_obj_get_ll(items[i]);
    }
}

static mp_obj_t property_get(struct mclw_instance *inst, size_t index) {
    struct mclw_value v;
    mclw_check(mclw_get(inst, index, &v));
    return value_to_obj(&inst->model->properties[index], &v);
}

static void property_set(struct mclw_instance *inst, size_t index, mp_obj_t obj) {
    struct mclw_value v;
    obj_to_value(&inst->model->properties[index], obj, &v);
    int err = mclw_set(inst, index, &v);
    if (err == -EINVAL) {
        mp_raise_ValueError(MP_ERROR_TEXT("value out of range"));
    }
    mclw_check(err);
}

static mp_obj_t instance_get(mp_obj_t self_in, mp_obj_t name_in) {
    struct mclw_instance *inst = mclw_instance_unwrap(self_in);
    int index = require_index(mclw_property_index(inst->model, mp_obj_str_get_str(name_in)), "property");
    return property_get(inst, (size_t)index);
}
static MP_DEFINE_CONST_FUN_OBJ_2(instance_get_obj, instance_get);

static mp_obj_t instance_set(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    (void)n_args;
    struct mclw_instance *inst = mclw_instance_unwrap(pos_args[0]);
    for (size_t i = 0; i < kw_args->alloc; i++) {
        if (!mp_map_slot_is_filled(kw_args, i)) {
            continue;
        }
        const char *key = mp_obj_str_get_str(kw_args->table[i].key);
        int index = require_index(mclw_property_index(inst->model, key), "property");
        property_set(inst, (size_t)index, kw_args->table[i].value);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(instance_set_obj, 1, instance_set);

static mp_obj_t instance_schema(mp_obj_t self_in) {
    return mclw_model_schema(mclw_instance_unwrap(self_in)->model);
}
static MP_DEFINE_CONST_FUN_OBJ_1(instance_schema_obj, instance_schema);

static mp_obj_t instance_faults(mp_obj_t self_in) {
    struct mclw_instance *inst = mclw_instance_unwrap(self_in);
    mp_obj_t *items = m_new(mp_obj_t, inst->model->fault_count + 1);
    for (size_t i = 0; i < inst->model->fault_count; i++) {
        items[i] = mp_obj_new_str_from_cstr(inst->model->faults[i]);
    }
    return mp_obj_new_tuple(inst->model->fault_count, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(instance_faults_obj, instance_faults);

static mp_obj_t instance_inject(size_t n_args, const mp_obj_t *args) {
    struct mclw_instance *inst = mclw_instance_unwrap(args[0]);
    int index = require_index(mclw_fault_index(inst->model, mp_obj_str_get_str(args[1])), "fault");
    mp_int_t count = n_args > 2 ? mp_obj_get_int(args[2]) : 1;
    if (count < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("count must not be negative"));
    }
    mclw_check(mclw_inject_fault(inst, (size_t)index, (uint32_t)count));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(instance_inject_obj, 2, 3, instance_inject);

static mp_obj_t instance_clear_faults(mp_obj_t self_in) {
    mclw_check(mclw_clear_faults(mclw_instance_unwrap(self_in)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(instance_clear_faults_obj, instance_clear_faults);

static mp_obj_t instance_read_region(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    struct mclw_instance *inst = mclw_instance_unwrap(args[0]);
    int index = require_index(mclw_region_index(inst->model, mp_obj_str_get_str(args[1])), "region");
    mp_int_t offset = mp_obj_get_int(args[2]);
    mp_int_t length = mp_obj_get_int(args[3]);
    if (offset < 0 || length < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("offset and length must not be negative"));
    }
    vstr_t vstr;
    vstr_init_len(&vstr, (size_t)length);
    mclw_check(mclw_read_region(inst, (size_t)index, (uint32_t)offset, vstr.buf, (size_t)length));
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(instance_read_region_obj, 4, 4, instance_read_region);

static mp_obj_t instance_write_region(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    struct mclw_instance *inst = mclw_instance_unwrap(args[0]);
    int index = require_index(mclw_region_index(inst->model, mp_obj_str_get_str(args[1])), "region");
    mp_int_t offset = mp_obj_get_int(args[2]);
    if (offset < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("offset must not be negative"));
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[3], &buf, MP_BUFFER_READ);
    mclw_check(mclw_write_region(inst, (size_t)index, (uint32_t)offset, buf.buf, buf.len));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(instance_write_region_obj, 4, 4, instance_write_region);

static mp_obj_t instance_trace(mp_obj_t self_in) {
    struct mclw_instance *inst = mclw_instance_unwrap(self_in);
    size_t count = inst->trace_count;
    mp_obj_t *items = m_new(mp_obj_t, count + 1);
    size_t start = (inst->trace_head + MCLW_TRACE_ENTRIES - count) % MCLW_TRACE_ENTRIES;
    for (size_t i = 0; i < count; i++) {
        const struct mclw_trace_entry *e = &inst->trace[(start + i) % MCLW_TRACE_ENTRIES];
        mp_obj_t fields[5] = {
            mp_obj_new_int_from_ull(e->t_us),
            mp_obj_new_str_from_cstr(mclw_trace_kind_name((enum mclw_trace_kind)e->kind)),
            mp_obj_new_int_from_uint(e->addr),
            MP_OBJ_NEW_SMALL_INT(e->len),
            MP_OBJ_NEW_SMALL_INT(e->result),
        };
        items[i] = mp_obj_new_tuple(5, fields);
    }
    return mp_obj_new_tuple(count, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(instance_trace_obj, instance_trace);

// Engine counters for one bus binding, by the binding's name. What the
// bus saw, from the HAL; the model never has to count.
static mp_obj_t instance_stats(mp_obj_t self_in, mp_obj_t name_in) {
    struct mclw_instance *inst = mclw_instance_unwrap(self_in);
    int index = require_index(mclw_binding_index(inst->model, mp_obj_str_get_str(name_in)), "binding");
    struct mclw_stat stats[MCLW_STATS_MAX];
    int n = mclw_binding_stats(inst, (size_t)index, stats, MCLW_STATS_MAX);
    if (n == -EINVAL) {
        mp_raise_ValueError(MP_ERROR_TEXT("binding not bound"));
    }
    if (n == -ENOTSUP) {
        mp_raise_ValueError(MP_ERROR_TEXT("binding has no stats"));
    }
    mclw_check(n);
    mp_obj_t d = mp_obj_new_dict((size_t)n);
    for (int i = 0; i < n; i++) {
        mp_obj_dict_store(d, mp_obj_new_str_from_cstr(stats[i].name), mp_obj_new_int_from_uint(stats[i].value));
    }
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_2(instance_stats_obj, instance_stats);

static mp_obj_t instance_reset(size_t n_args, const mp_obj_t *args) {
    struct mclw_instance *inst = mclw_instance_unwrap(args[0]);
    bool factory = n_args > 1 && mp_obj_is_true(args[1]);
    mclw_check(mclw_reset(inst, factory));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(instance_reset_obj, 1, 2, instance_reset);

static mp_obj_t instance_detach(mp_obj_t self_in) {
    mclw_check(mclw_detach(mclw_instance_unwrap(self_in)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(instance_detach_obj, instance_detach);

static const mp_rom_map_elem_t instance_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_get), MP_ROM_PTR(&instance_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&instance_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_schema), MP_ROM_PTR(&instance_schema_obj) },
    { MP_ROM_QSTR(MP_QSTR_faults), MP_ROM_PTR(&instance_faults_obj) },
    { MP_ROM_QSTR(MP_QSTR_inject), MP_ROM_PTR(&instance_inject_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_faults), MP_ROM_PTR(&instance_clear_faults_obj) },
    { MP_ROM_QSTR(MP_QSTR_read_region), MP_ROM_PTR(&instance_read_region_obj) },
    { MP_ROM_QSTR(MP_QSTR_write_region), MP_ROM_PTR(&instance_write_region_obj) },
    { MP_ROM_QSTR(MP_QSTR_trace), MP_ROM_PTR(&instance_trace_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&instance_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&instance_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_detach), MP_ROM_PTR(&instance_detach_obj) },
};
static MP_DEFINE_CONST_DICT(instance_locals, instance_locals_table);

// Properties as attributes. Anything that is not a property falls through
// to the method table via MP_OBJ_SENTINEL.
static void instance_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    instance_obj_t *o = MP_OBJ_TO_PTR(self_in);
    const char *name = qstr_str(attr);

    if (dest[0] == MP_OBJ_NULL) {
        if (attr == MP_QSTR_name) {
            dest[0] = mp_obj_new_str_from_cstr(o->inst->name);
            return;
        }
        if (attr == MP_QSTR_model) {
            dest[0] = mp_obj_new_str_from_cstr(o->inst->model->id);
            return;
        }
        if (attr == MP_QSTR_attached) {
            dest[0] = mp_obj_new_bool(o->inst->attached && o->inst->generation == o->generation);
            return;
        }
        int index = o->inst->model != NULL ? mclw_property_index(o->inst->model, name) : -1;
        if (index >= 0) {
            dest[0] = property_get(mclw_instance_unwrap(self_in), (size_t)index);
            return;
        }
        dest[1] = MP_OBJ_SENTINEL;
        return;
    }
    if (dest[1] != MP_OBJ_NULL) {
        int index = o->inst->model != NULL ? mclw_property_index(o->inst->model, name) : -1;
        if (index >= 0) {
            property_set(mclw_instance_unwrap(self_in), (size_t)index, dest[1]);
            dest[0] = MP_OBJ_NULL;
        }
    }
}

static void instance_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    instance_obj_t *o = MP_OBJ_TO_PTR(self_in);
    bool live = o->inst->attached && o->inst->generation == o->generation;
    mp_printf(print, "<Instance %s%s>", live ? o->inst->name : "", live ? "" : "detached");
}

MP_DEFINE_CONST_OBJ_TYPE(
    mclw_instance_type,
    MP_QSTR_Instance,
    MP_TYPE_FLAG_NONE,
    print, instance_print,
    attr, instance_attr,
    locals_dict, &instance_locals
    );
