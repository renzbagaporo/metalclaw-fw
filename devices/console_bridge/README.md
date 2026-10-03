<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# console_bridge

A USB-to-serial adapter on the DUT's console UART. The DUT finds a UART on
the other side of its pins; the host finds a serial port on the harness's
USB device and talks to the DUT through it, as it would through a USB
serial cable. A host program that speaks to a board over its serial port
(a firmware's console, a protobuf API, a bootloader) runs unchanged against
the DUT.

The model is one file with no bus code. Bytes from the host go to the UART
as fast as the UART takes them, and the USB function holds the host off
while it does not; bytes from the DUT go to a 4 KiB queue the harness
drains to the host. It runs with the simulation clock paused or running.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `uart` | `uart` | yes | a UART, from the tuple `(tx, rx)`, opened at 115200 baud 8N1 |
| 1 | `usb` | `usb_interface` | yes | a serial function (CDC ACM), string `Metalclaw DUT console` |

`usb` takes no `attach()` keyword; the runtime allocates the serial function
from the descriptor the model carries, and attaching or detaching
re-enumerates the harness once. The first serial function is interfaces 3
and 4 of the harness, whatever else is attached, so on Linux the port is

```text
/dev/serial/by-id/usb-Metalclaw_<product>_<serial>-if03
```

and on the bench
`/dev/serial/by-id/usb-Metalclaw_Metalclaw_E6612483CB313225-if03`. A
second bridge is `-if05`. Both carry the same interface string, which a
host reads from sysfs (`/sys/class/tty/ttyACM<n>/device/interface`).

On fixture 1 the DUT's console UART is the crossed pair, harness GP0 TX and
GP1 RX. Any other pin pair works too: the UART engine is a PL011 where the
pins carry one's functions and PIO otherwise.

```python
import metalclaw
console = metalclaw.attach("console_bridge", "dut_console", uart=(0, 1))
console.baud = 921600                  # the rate the DUT's UART runs at
console.follow_line_coding = True      # or take it from the host's port settings
console.dtr, console.to_host, console.from_host, console.overruns
console.stats("uart")
console.reset(factory=True)
console.detach()
```

## Properties

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `baud` | u32 | rw | baud | 300 .. 3 000 000 | the rate the UART runs at |
| `follow_line_coding` | bool | rw | | | the host's line coding is applied to the UART |
| `dtr` | bool | ro | | | the host holds DTR, which Linux does while the port is open |
| `rts` | bool | ro | | | the host holds RTS |
| `to_host` | u32 | ro | bytes | | DUT bytes queued for the host |
| `from_host` | u32 | ro | bytes | | host bytes handed to the UART |
| `overruns` | u32 | ro | bytes | | DUT bytes lost because the host queue was full while the host held DTR |
| `discarded` | u32 | ro | bytes | | DUT bytes lost because the host queue was full while it did not |
| `codings_refused` | u32 | ro | | | host line codings the UART could not take, while following |

`baud` defaults to 115200. A host's line coding (the baud, data bits, parity
and stop bits a program sets on the port) is ignored unless
`follow_line_coding` is set, because Linux's `cdc-acm` sets 9600 baud the
moment the port appears, and a program that opens it without choosing a
rate leaves it there. While following, every change the host makes,
and the setting itself, is applied once the UART's queue has drained; a
coding the UART cannot frame (mark or space parity, 1.5 stop bits, or
anything but 8N1 and 8N2 on a PIO UART) is counted in `codings_refused` and
leaves the UART as it was.

DTR and RTS are reported and do nothing else: they reset nothing, on the DUT
or here.

When the host opens the port (raises DTR) whatever the DUT sent before is
dropped from the queue, so a program does not start with a stale backlog;
only what the USB stack already held, 320 bytes at most, may still arrive
first, and a client that flushes its input after opening (pyserial does)
sees none of it. While the port stays closed the queue fills and the rest
is `discarded`.
With the port open, the host reads far faster than any UART, so `overruns`
stays at zero unless the host stops reading for longer than the queue
lasts (about 350 ms at 115200 baud).

`reset()` clears the counters. `reset(factory=True)` also puts the UART back
to 115200 8N1 and stops following the host's line coding. There are no
regions and no faults.

## The DUT image

`test/dut/rp2.overlay` enables `uart0` on GP0/GP1 at 115200 with the
console removed from it and names it `echo-uart`; `test/dut/rp2.conf` turns
on the serial driver, its interrupt-driven API and the ring buffer. The
fixture application then echoes every byte it receives on that UART
(`test/rig/dut/README.md`), so whatever the host writes to the bridge comes
back unchanged.

```sh
just build-dut console_bridge
METALCLAW_HARNESS_SERIAL=<serial> METALCLAW_ARM_TOOLCHAIN_DIR=<dir> \
METALCLAW_DUT_DEVICE=console_bridge just test hardware
```

## Tests

`test/cases.py` attaches the bridge on the fixture's UART, finds its port by
interface string and checks: the port is `-if03` and named for the console;
256 bytes come back through the DUT; a 32 KiB burst written as fast as USB
allows comes back whole, with no overrun anywhere and every byte counted
once each way; a backlog that built up while the port was closed is not
delivered on the next open; the host's line coding is ignored by default,
followed when asked, and a mark-parity coding is refused; and DTR and RTS
are reported while the DUT does not reboot.
