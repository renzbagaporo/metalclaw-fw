// SPDX-License-Identifier: Apache-2.0 OR MIT
// Prints the wire engines' programs as core/ports/rp2/hal/wire_programs.c
// encodes them for a harness clock and a model's facts, one program a line:
// its name, its side-set bits, then its words in hex. With `respond`, the
// TX words the responder program plays for the given pulses instead (hex,
// bit 31 for a low pulse, as hal/wire.h has them).
//
//   wire_programs_dump <harness_hz>
//   wire_programs_dump <harness_hz> respond <pulse>...
//   wire_programs_dump <sm_hz> envelope <word>...     what an ENVELOPE word reports
//   wire_programs_dump <harness_hz> enabled <passes>  LATCHED_IN's enabled ns

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metalclaw/bus.h"
#include "wire_programs.h"

static void print(const char *name, const struct wire_program *p) {
    printf("%s %u", name, (unsigned)p->side_bits);
    for (unsigned i = 0; i < p->length; i++) {
        printf(" %04x", (unsigned)p->words[i]);
    }
    printf("\n");
}

int main(int argc, char **argv) {
    struct wire_program p;
    uint32_t hz = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 125000000u;
    // The facts the ws2812b and ds18b20 models declare.
    struct mclw_wire_facts ws2812b = {
        .engine = MCLW_WIRE_PULSE_BITS_IN,
        .pins = 1,
        .pulse_bits = { .zero_min_ns = 250, .zero_max_ns = 550, .one_min_ns = 650, .one_max_ns = 950,
            .reset_ns = 50000 },
    };
    struct mclw_wire_facts ds18b20 = {
        .engine = MCLW_WIRE_ONEWIRE,
        .pins = 1,
        .onewire = { .sample_ns = 30000, .presence_wait_ns = 30000, .presence_ns = 120000 },
    };

    if (argc > 2 && strcmp(argv[2], "envelope") == 0) {
        for (int i = 3; i < argc; i++) {
            printf("%08x\n", (unsigned)wire_envelope_entry((uint32_t)strtoul(argv[i], NULL, 16), hz));
        }
        return 0;
    }
    if (argc > 2 && strcmp(argv[2], "enabled") == 0) {
        printf("%u\n", (unsigned)wire_latched_enabled_ns((uint32_t)strtoul(argv[3], NULL, 0), hz));
        return 0;
    }
    if (argc > 2 && strcmp(argv[2], "respond") == 0) {
        for (int i = 3; i < argc; i++) {
            uint32_t pulse = (uint32_t)strtoul(argv[i], NULL, 16);
            printf("%08x\n", (unsigned)wire_responder_word(pulse & 0x7fffffffu, (pulse >> 31) != 0u, hz));
        }
        return 0;
    }
    wire_encode_pulse_bits(&p, &ws2812b, hz);
    print("pulse_bits", &p);
    wire_encode_clocked_in(&p, 31);
    print("clocked_in", &p);
    wire_encode_latched_data(&p, 1);
    print("latched_data", &p);
    wire_encode_latched_enable(&p, 31);
    print("latched_enable", &p);
    wire_encode_onewire(&p, &ds18b20);
    print("onewire", &p);
    wire_encode_responder(&p);
    print("responder", &p);
    wire_encode_steps_out(&p, 2);
    print("steps_out", &p);
    wire_encode_envelope(&p);
    print("envelope", &p);
    printf("cycles %u %u\n", (unsigned)wire_to_cycles(50000, hz), (unsigned)wire_to_cycles(1000000, hz));
    return 0;
}
