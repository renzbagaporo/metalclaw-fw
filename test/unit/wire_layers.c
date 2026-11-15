// SPDX-License-Identifier: Apache-2.0 OR MIT
// The 1-Wire network layer (core/src/onewire.c) and the NEC decoder
// (devices/tsop382/nec.c) as a host library, for the PIO co-simulations in
// test_wire_engines.py to call where the firmware's slot handler and the
// IR model's poll would. The image answers like a DS18B20: Convert T (44h)
// busy until told, Read Scratchpad (BEh) from memory.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "metalclaw/hal/platform.h"
#include "metalclaw/onewire.h"

#include "nec.h"

uint32_t layers_onewire_init(const uint8_t *rom, const uint8_t *memory);
int layers_onewire_slot(int level);
void layers_onewire_reset(void);
void layers_onewire_status(int status);
uint32_t layers_onewire_take(void);
uint32_t layers_onewire_count(int which);
void layers_nec_init(void);
int layers_nec_take(int mark, uint32_t us);
uint32_t layers_nec_get(int which);

static struct mclw_onewire bus;
static struct nec_decoder nec;

static const struct mclw_onewire_function functions[] = {
    { .command = 0x44, .flags = MCLW_ONEWIRE_STATUS | MCLW_ONEWIRE_BUSY },
    { .command = 0xbe, .tx_offset = 0, .tx_length = 9 },
};

uint32_t mclw_hal_critical_enter(void) {
    return 0;
}

void mclw_hal_critical_exit(uint32_t state) {
    (void)state;
}

uint32_t layers_onewire_init(const uint8_t *rom, const uint8_t *memory) {
    mclw_onewire_init(&bus, rom, functions, 2);
    memcpy(bus.memory, memory, 9);
    bus.status = true;
    return sizeof(bus);
}

int layers_onewire_slot(int level) {
    return mclw_onewire_slot(&bus, level != 0) ? 1 : 0;
}

void layers_onewire_reset(void) {
    mclw_onewire_reset(&bus);
}

void layers_onewire_status(int status) {
    bus.status = status != 0;
}

uint32_t layers_onewire_take(void) {
    return mclw_onewire_take(&bus, NULL, NULL);
}

uint32_t layers_onewire_count(int which) {
    const uint32_t counts[] = { bus.counts.resets, bus.counts.selected, bus.counts.searches, bus.counts.matches,
        bus.counts.commands };
    return which >= 0 && which < 5 ? counts[which] : 0;
}

void layers_nec_init(void) {
    nec_init(&nec);
}

int layers_nec_take(int mark, uint32_t us) {
    return nec_take(&nec, mark != 0, us) ? 1 : 0;
}

uint32_t layers_nec_get(int which) {
    const uint32_t values[] = { nec.frames, nec.repeats, nec.errors, nec.last };
    return which >= 0 && which < 4 ? values[which] : 0;
}
