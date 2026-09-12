// SPDX-License-Identifier: Apache-2.0 OR MIT

#ifndef METALCLAW_DEVICE_SWD_PROBE_DAP_H
#define METALCLAW_DEVICE_SWD_PROBE_DAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "metalclaw/hal/swd.h"

#define METALCLAW_DAP_PACKET_SIZE 64u
#define METALCLAW_DAP_PACKET_COUNT 1u
#define METALCLAW_DAP_CLOCK_MIN_HZ 50000u
#define METALCLAW_DAP_CLOCK_MAX_HZ 10000000u

struct metalclaw_instance;

struct metalclaw_dap {
    struct metalclaw_instance *inst;
    struct metalclaw_swd *swd;
    uint32_t clock_hz;
    uint32_t transfers;
    uint32_t errors;
    uint16_t retry_count;
    uint8_t idle_cycles;
    uint8_t turnaround;
    bool data_phase;
    bool enabled;
    bool connected;
    uint8_t response[METALCLAW_DAP_PACKET_SIZE];
};

void metalclaw_dap_init(struct metalclaw_dap *dap, struct metalclaw_instance *inst,
    struct metalclaw_swd *swd);
int metalclaw_dap_enable(struct metalclaw_dap *dap, bool enable);
int metalclaw_dap_set_clock(struct metalclaw_dap *dap, uint32_t hz);
int metalclaw_dap_reset(struct metalclaw_dap *dap, bool factory);
size_t metalclaw_dap_process(struct metalclaw_dap *dap, const uint8_t *request, size_t request_length);

#endif
