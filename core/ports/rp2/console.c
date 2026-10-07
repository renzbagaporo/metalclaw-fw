// SPDX-License-Identifier: Apache-2.0 OR MIT
// The REPL as a builtin dupterm stream over the console HAL.

#include "py/runtime.h"
#include "py/stream.h"
#include "extmod/misc.h"
#include "metalclaw/hal/console.h"
#include "metalclaw_port.h"

typedef struct _console_obj_t {
    mp_obj_base_t base;
} console_obj_t;

static mp_uint_t console_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    (void)self_in;
    (void)errcode;
    return mclw_hal_console_write(buf, size);
}

static mp_uint_t console_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    (void)self_in;
    size_t n = mclw_hal_console_read(buf, size);
    if (n == 0) {
        *errcode = MP_EAGAIN;
        return MP_STREAM_ERROR;
    }
    return n;
}

static mp_uint_t console_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    (void)self_in;
    if (request != MP_STREAM_POLL) {
        *errcode = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
    mp_uint_t ret = 0;
    if ((arg & MP_STREAM_POLL_RD) && mclw_hal_console_readable()) {
        ret |= MP_STREAM_POLL_RD;
    }
    if ((arg & MP_STREAM_POLL_WR) && mclw_hal_console_writable()) {
        ret |= MP_STREAM_POLL_WR;
    }
    return ret;
}

static const mp_stream_p_t console_stream_p = {
    .read = console_read,
    .write = console_write,
    .ioctl = console_ioctl,
};

static MP_DEFINE_CONST_OBJ_TYPE(
    console_type,
    MP_QSTR_MetalclawConsole,
    MP_TYPE_FLAG_NONE,
    protocol, &console_stream_p
    );

static const console_obj_t console_obj = { { &console_type } };

bool mp_os_dupterm_is_builtin_stream(mp_const_obj_t stream) {
    return stream == MP_OBJ_FROM_PTR(&console_obj);
}

void mclw_console_vm_init(void) {
    MP_STATE_VM(dupterm_objs[0]) = MP_OBJ_FROM_PTR(&console_obj);
}
