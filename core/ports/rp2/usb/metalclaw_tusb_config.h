// SPDX-License-Identifier: Apache-2.0 OR MIT
// Selected with CFG_TUSB_CONFIG_FILE. CFG_TUSB_MCU and CFG_TUSB_OS come from
// pico-sdk's tinyusb_common_base and must not be set here.

#ifndef METALCLAW_TUSB_CONFIG_H
#define METALCLAW_TUSB_CONFIG_H

#include "py/mpconfig.h"

#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE      64

#define CFG_TUD_CDC                 1
#define CFG_TUD_CDC_EP_BUFSIZE      64
#define CFG_TUD_CDC_RX_BUFSIZE      256
#define CFG_TUD_CDC_TX_BUFSIZE      256

// One flash sector per transfer, so msc_disk.c never sees a partial block.
#define CFG_TUD_MSC                 1
#define CFG_TUD_MSC_EP_BUFSIZE      4096

#define CFG_TUD_VENDOR              0

#endif
