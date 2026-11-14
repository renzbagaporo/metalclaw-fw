// SPDX-License-Identifier: Apache-2.0 OR MIT
// Drives the swd_probe model's usb_rx on the host against a fake HAL whose
// vendor write takes only as many bytes as it is told to. Each argument is
// one DAP_Info(vendor) request: "all", or how many reply bytes the write
// takes. Prints one line per request, then one after reset().

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metalclaw/hal/platform.h"
#include "metalclaw/hal/swd.h"
#include "metalclaw/hal/usb.h"
#include "metalclaw/model.h"
#include "metalclaw/runtime.h"

#define PROP_SHORT_REPLIES 5u

extern const struct mclw_model mclw_model_swd_probe;

static size_t write_takes;
static size_t write_offered;
static size_t write_taken;
static uint8_t written[512];

size_t mclw_hal_usb_write(struct mclw_usb_interface *itf, const uint8_t *data, size_t len) {
    size_t n = len < write_takes ? len : write_takes;

    (void)itf;
    write_offered = len;
    write_taken = n;
    memcpy(written, data, n);
    return n;
}

const char *mclw_hal_usb_serial(void) {
    return "E6612483CB313225";
}

int mclw_hal_swd_port_on(struct mclw_swd *swd) {
    (void)swd;
    return 0;
}

int mclw_hal_swd_port_off(struct mclw_swd *swd) {
    (void)swd;
    return 0;
}

int mclw_hal_swd_set_clock(struct mclw_swd *swd, uint32_t hz) {
    (void)swd;
    (void)hz;
    return 0;
}

uint32_t mclw_hal_swd_get_clock(const struct mclw_swd *swd) {
    (void)swd;
    return 1000000u;
}

int mclw_hal_swd_configure(struct mclw_swd *swd, uint8_t turnaround, bool data_phase) {
    (void)swd;
    (void)turnaround;
    (void)data_phase;
    return 0;
}

int mclw_hal_swd_output_sequence(struct mclw_swd *swd, uint32_t count, const uint8_t *data) {
    (void)swd;
    (void)count;
    (void)data;
    return 0;
}

int mclw_hal_swd_input_sequence(struct mclw_swd *swd, uint32_t count, uint8_t *data) {
    (void)swd;
    (void)count;
    (void)data;
    return 0;
}

int mclw_hal_swd_transfer(struct mclw_swd *swd, uint8_t request, uint32_t *data,
    uint8_t idle_cycles, uint8_t *response) {
    (void)swd;
    (void)request;
    (void)data;
    (void)idle_cycles;
    *response = MCLW_SWD_ACK_OK;
    return 0;
}

int mclw_hal_swd_set_pins(struct mclw_swd *swd, uint8_t mask, uint8_t values) {
    (void)swd;
    (void)mask;
    (void)values;
    return 0;
}

int mclw_hal_swd_get_pins(struct mclw_swd *swd, uint8_t *values) {
    (void)swd;
    *values = 0u;
    return 0;
}

void mclw_hal_delay_us(uint32_t us) {
    (void)us;
}

void mclw_trace(struct mclw_instance *inst, enum mclw_trace_kind kind,
    uint32_t addr, uint16_t len, int16_t result) {
    (void)inst;
    (void)kind;
    (void)addr;
    (void)len;
    (void)result;
}

static long long short_replies(struct mclw_instance *inst) {
    struct mclw_value value = { .count = 1 };

    if (mclw_model_swd_probe.ops->get(inst, PROP_SHORT_REPLIES, &value) != 0) {
        return -1;
    }
    return (long long)value.v[0];
}

int main(int argc, char **argv) {
    static const uint8_t info_vendor[] = { 0x00u, 0x01u };
    static max_align_t state[64];
    static max_align_t fake_wire;
    static max_align_t fake_interface;
    struct mclw_instance inst;
    struct mclw_binding bindings[2];

    if (mclw_model_swd_probe.state_size > sizeof(state) ||
        strcmp(mclw_model_swd_probe.properties[PROP_SHORT_REPLIES].name, "short_replies") != 0) {
        return 2;
    }
    memset(&inst, 0, sizeof(inst));
    memset(bindings, 0, sizeof(bindings));
    inst.model = &mclw_model_swd_probe;
    inst.state = state;
    bindings[0].kind = MCLW_BINDING_SWD_WIRE;
    bindings[0].present = true;
    bindings[0].swd = (struct mclw_swd *)(void *)&fake_wire;
    bindings[1].kind = MCLW_BINDING_USB_INTERFACE;
    bindings[1].present = true;
    bindings[1].usb = (struct mclw_usb_interface *)(void *)&fake_interface;
    if (mclw_model_swd_probe.ops->init(&inst, bindings) != 0) {
        return 3;
    }
    for (int i = 1; i < argc; i++) {
        write_takes = strcmp(argv[i], "all") == 0 ? SIZE_MAX : (size_t)strtoul(argv[i], NULL, 0);
        write_offered = 0u;
        write_taken = 0u;
        (void)mclw_model_swd_probe.ops->usb_rx(&inst, 1u, info_vendor, sizeof(info_vendor));
        printf("offered=%zu taken=%zu short_replies=%lld reply=", write_offered, write_taken,
            short_replies(&inst));
        for (size_t b = 0; b < write_taken; b++) {
            printf("%02x", written[b]);
        }
        printf("\n");
    }
    if (mclw_model_swd_probe.ops->reset(&inst, false) != 0) {
        return 4;
    }
    printf("reset short_replies=%lld\n", short_replies(&inst));
    mclw_model_swd_probe.ops->deinit(&inst);
    return 0;
}
