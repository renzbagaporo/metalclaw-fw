// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_MODULE_H
#define MCLW_MODULE_H

#include "py/obj.h"
#include "metalclaw/runtime.h"

extern const mp_obj_type_t mclw_instance_type;

mp_obj_t mclw_instance_wrap(struct mclw_instance *inst);
struct mclw_instance *mclw_instance_unwrap(mp_obj_t obj);
mp_obj_t mclw_model_schema(const struct mclw_model *model);

// Raises for a negative errno; returns the value otherwise.
int mclw_check(int ret);

#endif
