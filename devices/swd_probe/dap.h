// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef MCLW_DEVICE_SWD_PROBE_DAP_H
#define MCLW_DEVICE_SWD_PROBE_DAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/swd.h"

#define MCLW_DAP_PACKET_SIZE 64u
#define MCLW_DAP_PACKET_COUNT 1u
#define MCLW_DAP_CLOCK_MIN_HZ 50000u
#define MCLW_DAP_CLOCK_MAX_HZ 10000000u

struct mclw_instance;

struct mclw_dap {
    struct mclw_instance *inst;
    struct mclw_swd *swd;
    uint32_t clock_hz;
    uint32_t transfers;
    uint32_t errors;
    uint16_t retry_count;
    uint8_t idle_cycles;
    uint8_t turnaround;
    bool data_phase;
    bool enabled;
    bool connected;
    uint8_t response[MCLW_DAP_PACKET_SIZE];
};

void mclw_dap_init(struct mclw_dap *dap, struct mclw_instance *inst,
    struct mclw_swd *swd);
int mclw_dap_enable(struct mclw_dap *dap, bool enable);
int mclw_dap_set_clock(struct mclw_dap *dap, uint32_t hz);
int mclw_dap_reset(struct mclw_dap *dap, bool factory);
size_t mclw_dap_process(struct mclw_dap *dap, const uint8_t *request, size_t request_length);

#endif
