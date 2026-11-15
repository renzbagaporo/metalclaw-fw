// SPDX-License-Identifier: Apache-2.0 OR MIT
// Winbond W25Q serial NOR flash, from the W25Q16JV datasheet (Revision D,
// August 12, 2016).

#ifndef MCLW_DEVICE_W25Q_H
#define MCLW_DEVICE_W25Q_H

#include "metalclaw/model.h"

// 8.1, Instruction Set Table 1: the instructions the model takes.
#define W25Q_WRITE_ENABLE 0x06
#define W25Q_WRITE_DISABLE 0x04
#define W25Q_READ_STATUS_1 0x05
#define W25Q_READ_DATA 0x03
#define W25Q_PAGE_PROGRAM 0x02
#define W25Q_SECTOR_ERASE 0x20
#define W25Q_BLOCK_ERASE_32K 0x52
#define W25Q_BLOCK_ERASE_64K 0xd8
#define W25Q_CHIP_ERASE 0xc7
#define W25Q_CHIP_ERASE_ALT 0x60
#define W25Q_POWER_DOWN 0xb9
#define W25Q_RELEASE_POWER_DOWN 0xab
#define W25Q_ENABLE_RESET 0x66
#define W25Q_RESET 0x99

// Status Register-1 (7.1): BUSY and WEL.
#define W25Q_SR1_BUSY 0x01
#define W25Q_SR1_WEL 0x02

#define W25Q_PAGE_SIZE 256u
#define W25Q_SECTOR_SIZE 4096u
// The array as far as the harness holds it: the first seven pages, which
// is what the SPI target's command image can serve.
#define W25Q_HELD_PAGES 7u
#define W25Q_HELD_BYTES (W25Q_HELD_PAGES * W25Q_PAGE_SIZE)

extern const struct mclw_model mclw_model_w25q16jv;

#endif
