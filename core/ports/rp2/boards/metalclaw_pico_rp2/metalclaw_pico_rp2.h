// SPDX-License-Identifier: Apache-2.0 OR MIT
// pico-sdk board header for the MetalClaw Pico RP2 v1.r1 carrier:
// https://github.com/renzbagaporo/metalclaw-pico-rp2 (tag v1.r1).

#ifndef _BOARDS_METALCLAW_PICO_RP2_H
#define _BOARDS_METALCLAW_PICO_RP2_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// RP2354B: the QFN-80 RP2350B with 2 MB of flash in the package.
#define PICO_RP2350A 0

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#define PICO_RP2350_A2_SUPPORTED 1

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#define PICO_FLASH_SPI_CLKDIV 2

// Every bank-0 GPIO but 47 (the PSRAM chip select) reaches the DUT: no LED
// and no default UART, I2C or SPI.

#endif
