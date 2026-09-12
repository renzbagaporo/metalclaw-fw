<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# USB

The harness owns its USB device. MicroPython's own USB device is switched off
(`MICROPY_HW_ENABLE_USBDEV 0`) and the port supplies the TinyUSB device stack,
its descriptors, and the REPL stream, on top of the unpatched upstream
`ports/rp2` build. The device comes up at boot, before the first `mp_init`,
with the base interface set. Devices that declare a `usb_interface` binding
add an interface when attached and remove it when detached; the port then
re-enumerates, once, after the changes settle.

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
| 3.. | vendor `FF/00/00`, bulk | `0x03`.. out, `0x83`.. in | one per attached device that declares one |

Up to `CFG_TUD_VENDOR` (2) device interfaces. The CDC notification endpoint
sits above them so their coming and going never moves it. The interface
string is the device's to choose; `swd_probe` uses `CMSIS-DAP v2`, which is
what probe-rs matches on.

On Linux the REPL appears as `/dev/serial/by-id/usb-MetalClaw_<product>_<serial>-if00`
and the volume as a raw FAT block device with no partition table.

## How it is wired into MicroPython

- `core/ports/rp2/metalclaw_port.h` is included by every board's
  `mpconfigboard.h`. It turns upstream USB off, sets the identity, and installs
  three hooks: `MICROPY_BOARD_EARLY_INIT` brings the device up once,
  `MICROPY_PORT_INIT_FUNC` registers the REPL stream on every `mp_init`, and
  `MICROPY_INTERNAL_EVENT_HOOK` pumps TinyUSB whenever the VM waits.
- `core/ports/rp2/usb/descriptors.c` rebuilds the descriptors from the open
  device interfaces; `usb/interfaces.c` implements `hal/usb.h` on TinyUSB's
  vendor class.
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

## Re-enumeration

Opening or closing a device interface marks the descriptor set dirty. 200 ms
after the last change the port rebuilds it; if the result is byte-identical to
what the host has, nothing happens. Otherwise it disconnects, waits 100 ms so
the host notices, and re-initialises with the new set. Both phases are driven
by an alarm, since a deinitialised device raises no interrupts to wake an idle
VM. The host loses its REPL session and mounted volume for about a second and
must reconnect by serial, not by device number.

A soft reset detaches every instance and then runs `main.py` again. If
`main.py` re-attaches the same devices, the rebuilt set is identical and there
is no re-enumeration. Cold boot with a USB-facing device in `main.py` costs
one, base set to full set.

`mp_init` keeps pending scheduler nodes across a soft reset, so the USB task
node is never touched by this code.

## Known limits

- `machine.lightsleep()` and `machine.deepsleep()` stop the USB clock when
  upstream USB is off, and the device drops off the bus. Do not use them.
- `_thread` is disabled. TinyUSB is only called from core 0's thread context.
- The MSC volume has no coherency with the device-side mount; see above.
- No MS OS 2.0 descriptors yet, so Windows needs a driver for later vendor
  interfaces. Not relevant while only CDC and MSC exist.
