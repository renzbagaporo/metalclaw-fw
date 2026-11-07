// SPDX-License-Identifier: Apache-2.0 OR MIT
// MetalClaw Pico RP2 v1.r1: RP2354B carrier for a Pico or Pico 2 DUT.

#define MICROPY_HW_BOARD_NAME                   "MetalClaw Pico RP2"
#define MICROPY_HW_FLASH_STORAGE_BYTES          (PICO_FLASH_SIZE_BYTES - 1024 * 1024)

// GPIO47 is the APS6404L PSRAM's chip select, pulled up on the board.
#define METALCLAW_RESERVED_PINS                 47,

// Every other GPIO reaches the DUT, so machine.I2C, SPI and UART have no
// default pins and take them as arguments.
#define MICROPY_HW_I2C_NO_DEFAULT_PINS          (1)
#define MICROPY_HW_SPI_NO_DEFAULT_PINS          (1)
#define MICROPY_HW_UART_NO_DEFAULT_PINS         (1)

#include "metalclaw_port.h"
