// SPDX-License-Identifier: Apache-2.0 OR MIT
// The HAL engines the host port does not have yet: opening one fails with
// -ENOTSUP, so a model that needs it refuses to attach and says so. Each
// moves to a file of its own as the bridge learns its bus (BRIDGE.md).

#include <errno.h>
#include <stddef.h>

#include "metalclaw/hal/swd.h"

// ---- SWD ---------------------------------------------------------------------

struct mclw_swd *mclw_hal_swd_open(uint8_t swclk, uint8_t swdio, uint8_t nreset, const void *owner, int *err) {
    (void)swclk;
    (void)swdio;
    (void)nreset;
    (void)owner;
    *err = -ENOTSUP;
    return NULL;
}

void mclw_hal_swd_close(struct mclw_swd *swd) {
    (void)swd;
}

int mclw_hal_swd_port_on(struct mclw_swd *swd) {
    (void)swd;
    return -ENOTSUP;
}

int mclw_hal_swd_port_off(struct mclw_swd *swd) {
    (void)swd;
    return -ENOTSUP;
}

int mclw_hal_swd_set_clock(struct mclw_swd *swd, uint32_t hz) {
    (void)swd;
    (void)hz;
    return -ENOTSUP;
}

uint32_t mclw_hal_swd_get_clock(const struct mclw_swd *swd) {
    (void)swd;
    return 0;
}

int mclw_hal_swd_configure(struct mclw_swd *swd, uint8_t turnaround, bool data_phase) {
    (void)swd;
    (void)turnaround;
    (void)data_phase;
    return -ENOTSUP;
}

int mclw_hal_swd_output_sequence(struct mclw_swd *swd, uint32_t count, const uint8_t *data) {
    (void)swd;
    (void)count;
    (void)data;
    return -ENOTSUP;
}

int mclw_hal_swd_input_sequence(struct mclw_swd *swd, uint32_t count, uint8_t *data) {
    (void)swd;
    (void)count;
    (void)data;
    return -ENOTSUP;
}

int mclw_hal_swd_transfer(struct mclw_swd *swd, uint8_t request, uint32_t *data, uint8_t idle_cycles,
    uint8_t *response) {
    (void)swd;
    (void)request;
    (void)data;
    (void)idle_cycles;
    (void)response;
    return -ENOTSUP;
}

int mclw_hal_swd_set_pins(struct mclw_swd *swd, uint8_t mask, uint8_t values) {
    (void)swd;
    (void)mask;
    (void)values;
    return -ENOTSUP;
}

int mclw_hal_swd_get_pins(struct mclw_swd *swd, uint8_t *values) {
    (void)swd;
    (void)values;
    return -ENOTSUP;
}
