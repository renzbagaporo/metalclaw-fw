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
| Manufacturer | `Metalclaw` |
| Product | `Metalclaw`, unless a board overrides it |
| Serial | the RP2040 unique id, 16 uppercase hex digits |

## Interfaces

| Interface | Class | Endpoints | Purpose |
| --- | --- | --- | --- |
| 0, 1 | CDC ACM | `0x85` notify, `0x01` out, `0x81` in | MicroPython REPL |
| 2 | MSC, SCSI, bulk-only | `0x02` out, `0x82` in | the flash filesystem |
| 3, 4 and 5, 6 | CDC ACM | `0x88`, `0x89` notify, `0x06`, `0x07` out, `0x86`, `0x87` in | serial functions, one per attached device that declares one |
| after them | vendor `FF/00/00`, bulk | `0x03`.. out, `0x83`.. in | vendor interfaces, one per attached device that declares one |

A device declares a `usb_interface` binding with a function kind and an
interface string. Up to two serial functions (`CFG_TUD_CDC` is 3, the REPL
included) and `CFG_TUD_VENDOR` (2) vendor interfaces may be attached at
once. Serial functions come first, in the order their slots were taken, so
the first one is always interfaces 3 and 4 whether or not a vendor
interface exists; vendor interfaces follow and their numbers move when a
serial function comes or goes. Endpoint numbers are fixed per position,
and the REPL's notification endpoint sits above the vendor range so
nothing ever moves it. The interface string is the device's to choose;
`swd_probe` uses `CMSIS-DAP v2`, which is what probe-rs matches on.

On Linux the REPL appears as `/dev/serial/by-id/usb-Metalclaw_<product>_<serial>-if00`,
a serial function as the same name ending in its control interface number
in two hex digits: the first one is `-if03`, the second `-if05`. On the
bench that is
`/dev/serial/by-id/usb-Metalclaw_Metalclaw_E6612483CB313225-if03`.
The volume is a raw FAT block device with no partition table.

## Serial functions

A serial function is a CDC ACM port whose bytes belong to the device that
opened it, not to MicroPython:

- Host to device: the port reads what TinyUSB received into a 64-byte stage
  and offers it to the device (`usb_rx`, USB task, runtime lock). The device
  says how much it took; the rest stays staged and is offered again, and
  TinyUSB does not re-arm the OUT endpoint while its own FIFO is full, so a
  device that cannot keep up holds the host off instead of losing bytes.
- Device to host: `mclw_hal_usb_write()` copies into a 4 KiB queue per
  function from any thread context, the worker included; the USB task moves
  it into TinyUSB. A short return means the queue was full. When the host
  opens the port (raises DTR) the queue and TinyUSB's FIFO are emptied, so
  what waited for a host is not replayed to the next one. Linux submits its
  reads before it raises DTR, so what TinyUSB already held may still go
  out first: its 256-byte FIFO and one 64-byte packet at most. A client
  that flushes its input after opening, as pyserial does, sees none of it.
  While the host cannot reach the function (not configured, re-enumerating)
  DTR and RTS read as low.
- Line coding and the DTR and RTS lines are cached when the host sets them
  and the device is told (`usb_control`, USB task, runtime lock); it reads
  them with `mclw_hal_usb_line_coding()` and
  `mclw_hal_usb_control_lines()`. Nothing in the port acts on them.
- Neither a queued byte nor a staged one raises a USB interrupt, so while a
  serial function is open a 1 ms timer on core 0 schedules the USB task
  whenever either is waiting.

## How it is wired into MicroPython

- `core/ports/rp2/metalclaw_port.h` is included by every board's
  `mpconfigboard.h`. It turns upstream USB off, sets the identity, and installs
  three hooks: `MICROPY_BOARD_EARLY_INIT` brings the device up once,
  `MICROPY_PORT_INIT_FUNC` registers the REPL stream on every `mp_init`, and
  `MICROPY_INTERNAL_EVENT_HOOK` pumps TinyUSB whenever the VM waits.
- `core/ports/rp2/usb/descriptors.c` rebuilds the descriptors from the open
  device functions; `usb/interfaces.c` implements `hal/usb.h` on TinyUSB's
  vendor and CDC classes.
- `core/ports/rp2/usb/owner.c` initialises TinyUSB, pumps it from a scheduler
  node whenever the USB interrupt reports an event, and feeds CDC input into
  MicroPython's `stdin_ringbuf` with Ctrl-C handling.
- `core/ports/rp2/console.c` is the REPL: a builtin dupterm stream over
  `hal/console.h`, whose read and write go straight to the CDC interface
  with no allocation.
- MSC is served by upstream's `msc_disk.c`, exposing the same flash region
  MicroPython mounts as `/`. There is no coherency between the two sides yet:
  a host write is only seen by MicroPython after a soft reset, and the host
  should not write while MicroPython does.
- TinyUSB reads its configuration from `core/ports/rp2/usb/metalclaw_tusb_config.h`,
  selected with `CFG_TUSB_CONFIG_FILE`.

## Re-enumeration

Opening or closing a device function marks the descriptor set dirty. 200 ms
after the last change the port rebuilds it; if the result is byte-identical to
what the host has, nothing happens. Otherwise it removes the pull-up, waits
100 ms so the host notices, rebuilds the set once more in case an interface
opened or closed meanwhile, and restores it; the host's bus reset on reconnect
resets the class drivers and the next `SET_CONFIGURATION` opens the new set.
TinyUSB is never deinitialised: on this port `tud_init` claims two hardware
spinlocks and `tud_deinit` releases one. Both phases are driven by an alarm,
since a device off the bus raises no interrupts to wake an idle VM. The host
loses its REPL session and mounted volume for about a second and must
reconnect by serial, not by device number.

A soft reset detaches every instance and then runs `main.py` again. If
`main.py` re-attaches the same devices, the rebuilt set is identical and there
is no re-enumeration; a function that comes back exactly where the host
already has it is served from the moment it opens, any other new function
from when the host has the new set. Cold boot with a USB-facing device in `main.py` costs
one, base set to full set.

`mp_init` keeps pending scheduler nodes across a soft reset, so the USB task
node is never touched by this code.

## Known limits

- `machine.lightsleep()` and `machine.deepsleep()` stop the USB clock when
  upstream USB is off, and the device drops off the bus. Do not use them.
- `_thread` is disabled: core 1 is the engine worker. TinyUSB is only called
  from core 0's thread context.
- The MSC volume has no coherency with the device-side mount; see above.
- No MS OS 2.0 descriptors yet, so Windows binds no driver to a vendor
  interface: the `swd_probe` CMSIS-DAP v2 interface needs WinUSB bound to it
  by hand before probe-rs can use it.
