// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef METALCLAW_MODULE_H
#define METALCLAW_MODULE_H

#include "py/obj.h"
#include "metalclaw/runtime.h"

extern const mp_obj_type_t metalclaw_instance_type;

mp_obj_t metalclaw_instance_wrap(struct metalclaw_instance *inst);
struct metalclaw_instance *metalclaw_instance_unwrap(mp_obj_t obj);
mp_obj_t metalclaw_model_schema(const struct metalclaw_model *model);

// Raises for a negative errno; returns the value otherwise.
int metalclaw_check(int ret);

#endif
