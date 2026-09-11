// SPDX-License-Identifier: Apache-2.0 OR MIT
// The `metalclaw` module. Currently only reports its version.

#include "py/obj.h"
#include "py/runtime.h"

#define METALCLAW_VERSION "0.0.0"

static mp_obj_t metalclaw_version(void) {
    return mp_obj_new_str_from_cstr(METALCLAW_VERSION);
}
static MP_DEFINE_CONST_FUN_OBJ_0(metalclaw_version_obj, metalclaw_version);

static const mp_rom_map_elem_t metalclaw_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_metalclaw) },
    { MP_ROM_QSTR(MP_QSTR_version), MP_ROM_PTR(&metalclaw_version_obj) },
};
static MP_DEFINE_CONST_DICT(metalclaw_module_globals, metalclaw_module_globals_table);

const mp_obj_module_t metalclaw_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&metalclaw_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_metalclaw, metalclaw_module);
