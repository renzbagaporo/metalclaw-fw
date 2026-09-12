// SPDX-License-Identifier: Apache-2.0 OR MIT
// Included by every MetalClaw rp2 board's mpconfigboard.h. Board files add
// only their identity on top of this.

#ifndef METALCLAW_PORT_RP2_H
#define METALCLAW_PORT_RP2_H

#include <stdbool.h>
#include <stddef.h>

// MicroPython's own USB device is switched off so the port can own the
// composite device. Its descriptor callbacks are non-weak, so there is no
// way to extend them in place.
#define MICROPY_HW_ENABLE_USBDEV                (0)
#define MICROPY_HW_ENABLE_UART_REPL             (0)

// Still needed with USBDEV off: selects _boot_fat.py and pins the FatFS
// sector size to the flash sector so upstream's msc_disk.c compiles.
#define MICROPY_HW_USB_MSC                      (1)
#define MICROPY_HW_USB_MSC_INQUIRY_VENDOR_STRING    "MetalClw"
#define MICROPY_HW_USB_MSC_INQUIRY_PRODUCT_STRING   "MetalClaw Volume"
#define MICROPY_HW_USB_MSC_INQUIRY_REVISION_STRING  "1.00"

#define MICROPY_HW_USB_VID                      (0x2FE3)
#define MICROPY_HW_USB_PID                      (0x0204)
#define MICROPY_HW_USB_MANUFACTURER_STRING      "MetalClaw"
#ifndef MICROPY_HW_USB_PRODUCT_FS_STRING
#define MICROPY_HW_USB_PRODUCT_FS_STRING        "MetalClaw"
#endif
#define MICROPY_HW_USB_CDC_INTERFACE_STRING     "MetalClaw REPL"
#define MICROPY_HW_USB_MSC_INTERFACE_STRING     "METALCLAW"

// The REPL is a builtin dupterm stream over the owned CDC interface.
#define MICROPY_PY_OS_DUPTERM_BUILTIN_STREAM    (1)

// TinyUSB class calls are only safe from core 0's thread context, where
// tud_task also runs.
#define MICROPY_PY_THREAD                       (0)

void metalclaw_usb_init(void);
void metalclaw_usb_task(void);

size_t metalclaw_console_write(const void *buf, size_t size);
size_t metalclaw_console_read(void *buf, size_t size);
bool metalclaw_console_readable(void);
bool metalclaw_console_writable(void);
void metalclaw_console_vm_init(void);
void metalclaw_detach_all(void);

#define MICROPY_BOARD_EARLY_INIT()              metalclaw_usb_init()
#define MICROPY_PORT_INIT_FUNC                  metalclaw_console_vm_init()
#define MICROPY_PORT_DEINIT_FUNC                metalclaw_detach_all()
#define MICROPY_INTERNAL_EVENT_HOOK             metalclaw_usb_task()

#endif
