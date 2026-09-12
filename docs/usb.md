<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# USB

The harness owns its USB device. MicroPython's own USB device is switched off
(`MICROPY_HW_ENABLE_USBDEV 0`) and the port supplies the TinyUSB device stack,
its descriptors, and the REPL stream, on top of the unpatched upstream
`ports/rp2` build. The device is brought up once at boot, before the first
`mp_init`, and is never torn down: a MicroPython soft reset leaves it
enumerated.

## Identity

| Field | Value |
| --- | --- |
| VID:PID | `2FE3:0204` |
| Manufacturer | `MetalClaw` |
| Product | per board, e.g. `MetalClaw Pico` |
| Serial | the RP2040 unique id, 16 uppercase hex digits |

## Interfaces

| Interface | Class | Endpoints | Purpose |
| --- | --- | --- | --- |
| 0, 1 | CDC ACM | `0x85` notify, `0x01` out, `0x81` in | MicroPython REPL |
| 2 | MSC, SCSI, bulk-only | `0x02` out, `0x82` in | the flash filesystem |

Endpoints `0x83` and `0x84` are reserved for the simulation and CMSIS-DAP
interfaces that come later. The CDC notification endpoint sits above them so
adding those interfaces moves no address the host has already learned.

On Linux the REPL appears as `/dev/serial/by-id/usb-MetalClaw_<product>_<serial>-if00`
and the volume as a raw FAT block device with no partition table.

## How it is wired into MicroPython

- `core/ports/rp2/metalclaw_port.h` is included by every board's
  `mpconfigboard.h`. It turns upstream USB off, sets the identity, and installs
  three hooks: `MICROPY_BOARD_EARLY_INIT` brings the device up once,
  `MICROPY_PORT_INIT_FUNC` registers the REPL stream on every `mp_init`, and
  `MICROPY_INTERNAL_EVENT_HOOK` pumps TinyUSB whenever the VM waits.
- `core/ports/rp2/usb/descriptors.c` holds the descriptors.
- `core/ports/rp2/usb/owner.c` initialises TinyUSB, pumps it from a scheduler
  node whenever the USB interrupt reports an event, and feeds CDC input into
  MicroPython's `stdin_ringbuf` with Ctrl-C handling.
- `core/micropython/console.c` is the REPL: a builtin dupterm stream whose
  read and write go straight to the CDC interface with no allocation.
- MSC is served by upstream's `msc_disk.c`, exposing the same flash region
  MicroPython mounts as `/`. There is no coherency between the two sides yet:
  a host write is only seen by MicroPython after a soft reset, and the host
  should not write while MicroPython does.
- TinyUSB reads its configuration from `core/ports/rp2/usb/metalclaw_tusb_config.h`,
  selected with `CFG_TUSB_CONFIG_FILE`.

## Soft reset

`mp_init` keeps pending scheduler nodes across a soft reset, so the USB task
node is never touched by this code. The device stays enumerated; only the REPL
stream is re-registered.

## Known limits

- `machine.lightsleep()` and `machine.deepsleep()` stop the USB clock when
  upstream USB is off, and the device drops off the bus. Do not use them.
- `_thread` is disabled. TinyUSB is only called from core 0's thread context.
- The MSC volume has no coherency with the device-side mount; see above.
- No MS OS 2.0 descriptors yet, so Windows needs a driver for later vendor
  interfaces. Not relevant while only CDC and MSC exist.
