// SPDX-License-Identifier: Apache-2.0 OR MIT
// Bus facts a datasheet states about a part. A model declares them in its
// binding descriptors; the fixture side reads them from the schema. The
// master-side settings (clock, baud in use) are the DUT author's business
// and never appear in device code.

#ifndef MCLW_BUS_H
#define MCLW_BUS_H

#include <stdbool.h>
#include <stdint.h>

// Bit n set: the part accepts SPI mode n (CPOL = n >> 1, CPHA = n & 1).
#define MCLW_SPI_MODE_BIT(n) (1u << (n))

struct mclw_spi_facts {
    uint8_t modes;
    uint8_t default_mode;
    bool msb_first;
};

enum mclw_uart_parity {
    MCLW_UART_PARITY_NONE,
    MCLW_UART_PARITY_EVEN,
    MCLW_UART_PARITY_ODD,
};

struct mclw_uart_format {
    uint8_t data_bits;
    uint8_t stop_bits;
    enum mclw_uart_parity parity;
};

struct mclw_uart_facts {
    uint32_t default_baud;
    struct mclw_uart_format format;
};

// A wire: a few pins whose timing the part defines, run by one of the
// port's wire engines (metalclaw/hal/wire.h says what each one does). The
// model names the engine and gives it the datasheet's times, in ns.
enum mclw_wire_engine {
    MCLW_WIRE_NONE,
    MCLW_WIRE_PULSE_BITS_IN,
    MCLW_WIRE_CLOCKED_IN,
    MCLW_WIRE_LATCHED_IN,
    MCLW_WIRE_PINS,
    MCLW_WIRE_ONEWIRE,
    MCLW_WIRE_RESPONDER,
    MCLW_WIRE_STEPS_OUT,
    MCLW_WIRE_ENVELOPE,
};

struct mclw_wire_facts {
    enum mclw_wire_engine engine;
    // How many pins the binding takes; PINS takes one to this many.
    uint8_t pins;
    union {
        // A high pulse of zero_min..zero_max is a 0, one of
        // one_min..one_max a 1; a low of reset or longer ends the frame.
        struct {
            uint32_t zero_min_ns;
            uint32_t zero_max_ns;
            uint32_t one_min_ns;
            uint32_t one_max_ns;
            uint32_t reset_ns;
        } pulse_bits;
        // When the part samples a write slot and how long it holds a 0 in
        // a read slot, both from the slot's falling edge; how long it
        // waits after a reset's rising edge and how long its presence
        // pulse is.
        struct {
            uint32_t sample_ns;
            uint32_t presence_wait_ns;
            uint32_t presence_ns;
        } onewire;
        // A host low this long or longer is a start.
        struct {
            uint32_t start_min_ns;
        } responder;
        // No carrier edge for this long ends a burst.
        struct {
            uint32_t gap_ns;
        } envelope;
    };
};

#endif
