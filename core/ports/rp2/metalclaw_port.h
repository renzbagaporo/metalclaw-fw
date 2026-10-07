// SPDX-License-Identifier: Apache-2.0 OR MIT
// Included by every Metalclaw rp2 board's mpconfigboard.h. Board files add
// only their identity on top of this.

#ifndef MCLW_PORT_RP2_H
#define MCLW_PORT_RP2_H

#include "metalclaw/runtime.h"

// MicroPython's own USB device is switched off so the port can own the
// composite device. Its descriptor callbacks are non-weak, so there is no
// way to extend them in place.
#define MICROPY_HW_ENABLE_USBDEV                (0)
#define MICROPY_HW_ENABLE_UART_REPL             (0)

// Still needed with USBDEV off: selects _boot_fat.py and pins the FatFS
// sector size to the flash sector so upstream's msc_disk.c compiles.
#define MICROPY_HW_USB_MSC                      (1)
#define MICROPY_HW_USB_MSC_INQUIRY_VENDOR_STRING    "Generic"
#define MICROPY_HW_USB_MSC_INQUIRY_PRODUCT_STRING   "Metalclaw Volume"
#define MICROPY_HW_USB_MSC_INQUIRY_REVISION_STRING  "1.00"

#define MICROPY_HW_USB_VID                      (0x2FE3)
#define MICROPY_HW_USB_PID                      (0x0204)
#define MICROPY_HW_USB_MANUFACTURER_STRING      "Metalclaw"
#ifndef MICROPY_HW_USB_PRODUCT_FS_STRING
#define MICROPY_HW_USB_PRODUCT_FS_STRING        "Metalclaw"
#endif
#define MICROPY_HW_USB_CDC_INTERFACE_STRING     "Metalclaw REPL"
#define MICROPY_HW_USB_MSC_INTERFACE_STRING     "METALCLAW"

// The REPL is a builtin dupterm stream over the owned CDC interface.
#define MICROPY_PY_OS_DUPTERM_BUILTIN_STREAM    (1)

// Pins no device may ever claim, as a comma-terminated list, e.g. `2, 3,`.
#ifndef METALCLAW_RESERVED_PINS
#define METALCLAW_RESERVED_PINS
#endif

// Core 1 is the engine worker (hal/worker.c), so MicroPython gets no
// threads; TinyUSB is only ever called from core 0's thread context.
#define MICROPY_PY_THREAD                       (0)

void mclw_platform_init(void);
void mclw_worker_start(void);
void mclw_usb_init(void);
void mclw_usb_task(void);
void mclw_port_event_hook(void);
void mclw_console_vm_init(void);
void mclw_port_early_init(void);

#define MICROPY_BOARD_EARLY_INIT()              mclw_port_early_init()
#define MICROPY_PORT_INIT_FUNC                  mclw_console_vm_init()
#define MICROPY_PORT_DEINIT_FUNC                mclw_detach_all()
#define MICROPY_INTERNAL_EVENT_HOOK             mclw_port_event_hook()

#endif
