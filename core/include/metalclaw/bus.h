// SPDX-License-Identifier: Apache-2.0 OR MIT
// Bus facts a datasheet states about a part. A model declares them in its
// binding descriptors; the fixture side reads them from the schema. The
// master-side settings (clock, baud in use) are the DUT author's business
// and never appear in device code.

#ifndef METALCLAW_BUS_H
#define METALCLAW_BUS_H

#include <stdbool.h>
#include <stdint.h>

// Bit n set: the part accepts SPI mode n (CPOL = n >> 1, CPHA = n & 1).
#define METALCLAW_SPI_MODE_BIT(n) (1u << (n))

struct metalclaw_spi_facts {
    uint8_t modes;
    uint8_t default_mode;
    bool msb_first;
};

enum metalclaw_uart_parity {
    METALCLAW_UART_PARITY_NONE,
    METALCLAW_UART_PARITY_EVEN,
    METALCLAW_UART_PARITY_ODD,
};

struct metalclaw_uart_format {
    uint8_t data_bits;
    uint8_t stop_bits;
    enum metalclaw_uart_parity parity;
};

struct metalclaw_uart_facts {
    uint32_t default_baud;
    struct metalclaw_uart_format format;
};

#endif
