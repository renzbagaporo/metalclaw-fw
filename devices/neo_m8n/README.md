<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# neo_m8n

A u-blox NEO-M8N GNSS receiver on UART. The harness speaks the UBX protocol
the part's datasheet describes, so the board under test can bind Zephyr's
in-tree `u-blox,m8` GNSS driver to it and receive position fixes through the
GNSS API. The host sets a position, velocity, fix state and UTC time; the
model answers the driver's configuration and reports that fix in a
`NAV-PVT` every fix interval of the simulation clock.

The model is one file with no bus code. It parses UBX frames out of whatever
the UART engine delivers, answers each one with `send()`, and emits `NAV-PVT`
from `time_advanced`. Both run in thread context; the model never sees an
interrupt.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `uart` | `uart` | yes | a UART, from the tuple `(tx, rx)` |

The binding advertises the datasheet's default port setting, 9600 baud 8N1.
On fixture 1 the crossed pair is GP0/GP1:

```python
import metalclaw
gnss = metalclaw.attach("neo_m8n", "gnss", uart=(0, 1))
gnss.set(latitude=473977420, longitude=-1223321500, altitude=52000, speed=1500,
         heading=9000000, satellites=9, fix=3, time=(2026, 9, 13, 5, 42, 7))
metalclaw.advance_us(1000000)          # one fix interval: one NAV-PVT
gnss.baud, gnss.rate_ms, gnss.nav_pvt_rate, gnss.running, gnss.fixes
gnss.stats("uart")
gnss.reset()
gnss.detach()
```

## Properties

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `latitude` | i32 | rw | 1e-7 deg | ±90e7 | `NAV-PVT.lat` |
| `longitude` | i32 | rw | 1e-7 deg | ±180e7 | `NAV-PVT.lon` |
| `altitude` | i32 | rw | mm | | `NAV-PVT.hMSL`, also sent as `height` |
| `speed` | u32 | rw | mm/s | | `NAV-PVT.gSpeed` |
| `heading` | i32 | rw | 1e-5 deg | 0 .. 359.99999° | `NAV-PVT.headMot` and `headVeh` |
| `satellites` | u32 | rw | | 0 .. 32 | `NAV-PVT.numSV` |
| `fix` | u32 | rw | | 0 .. 5 | `NAV-PVT.fixType`; 0 is no fix, 3 a 3D fix; any fix sets `gnssFixOK` and the `valid` bits |
| `pdop` | u32 | rw | 0.01 | | `NAV-PVT.pDOP`; the driver reports it as `hdop` ×10 |
| `time` | i32[6] | rw | | | UTC year, month, day, hour, minute, second; a set is the time the next fix carries, and from that fix on it runs with the simulation clock |
| `baud` | u32 | rw | baud | 4800 .. 921600 | the rate the port listens at; a `CFG-PRT` from the DUT moves it too |
| `rate_ms` | u32 | ro | ms | | `measRate` as the DUT left it with `CFG-RATE` |
| `nav_pvt_rate` | u32 | ro | | | the `NAV-PVT` output rate the DUT set with `CFG-MSG`; 0 until it does |
| `dyn_model` | u32 | ro | | | `CFG-NAV5.dynModel` as the DUT left it |
| `running` | bool | ro | | | cleared by a `CFG-RST` stop, set by a start |
| `resets` | u32 | ro | | | `CFG-RST` frames received |
| `fixes` | u32 | ro | | | `NAV-PVT` frames sent |

At power-on there is no fix: position, speed and heading are zero, `fix` is
0, `pdop` 2.50, the first fix carries 2015-01-01 00:00:00, and the port is at
9600 baud with `NAV-PVT` output off, as on the real part. `reset()` returns to
that state, including the baud rate; `factory` changes nothing further.
There are no regions and no faults.

## The protocol

Every frame is `B5 62`, class, id, 16-bit little-endian length, payload and
the 8-bit Fletcher checksum over class through payload. Frames with a bad
checksum are dropped without a reply, as the part does. Payloads longer than
64 bytes are consumed and answered with `ACK-NAK`.

| frame | on a poll (empty payload) | on a set |
| --- | --- | --- |
| `MON-VER` (0A 04) | 40-byte reply: `ROM CORE 3.01 (107888)`, hardware `00080000` | — |
| `MON-GNSS` (0A 28) | GPS, GLONASS, BeiDou and Galileo supported; GPS and GLONASS enabled; two at once | — |
| `CFG-PRT` (06 00) | the UART port: 8N1, the current baud, UBX in and out | `ACK-ACK`, then the port moves to the payload's baud rate |
| `CFG-MSG` (06 01) | `ACK-NAK` | `ACK-ACK`; a rate for `NAV-PVT` is remembered, any other message is accepted and ignored |
| `CFG-RST` (06 04) | — | no acknowledgement, as on the part; `resetMode` 0x08 stops the receiver, 0x09 starts it |
| `CFG-RATE` (06 08) | `measRate`, `navRate`, `timeRef` | `ACK-ACK` and the new `measRate`; below 25 ms `ACK-NAK` |
| `CFG-NAV5` (06 24) | `dynModel` and `fixMode` auto | `ACK-ACK`; `dynModel` taken when its apply bit is set |
| anything else | `ACK-NAK` | `ACK-NAK` |

The baud switch after `CFG-PRT` happens once the acknowledgement has left at
the old rate; the engine's `set_baud` drains the transmit queue first.

`NAV-PVT` (01 07, 92 bytes) goes out from `time_advanced`: when the
simulation clock reaches the next fix interval (`rate_ms`), the receiver is
running and `nav_pvt_rate` is non-zero, one frame is sent. Fixes fall on a
grid of `rate_ms` from the first, so the UTC they carry stays on whole
intervals; a jump of several intervals in one `advance_us()` produces one
frame, not several, and the grid restarts from it. The UTC fields and
`nano` are the fix's own time. `iTOW` counts `fixes × rate_ms`. Nothing is sent unsolicited before the DUT enables the message,
which also keeps Zephyr's UBX layer safe: it dereferences a script that does
not exist yet if a frame arrives before its first poll.

## The Zephyr driver this is built against

Zephyr v4.4.0, `drivers/gnss/u_blox/gnss_u_blox_m8.c` with the interrupt
driven UART backend (`CONFIG_GNSS=y` selects the rest). Its init polls
`MON-VER` at the UART node's `current-speed` for 200 ms; without an answer
it drops the UART to `initial-baudrate`, sends `CFG-PRT` asking for
`current-speed`, and comes back up. Then, at the working rate: `MON-VER`
again, `CFG-RST` stop, a one-second sleep, `CFG-RATE` with the node's
`fix-rate`, thirteen `CFG-MSG` frames turning NMEA sentences off, `CFG-MSG`
turning `NAV-PVT` on, and `CFG-RST` start. Every set waits for an `ACK-ACK`
(three seconds, three attempts, re-sending each time). A receiver that
never answers costs the DUT about six seconds of boot and leaves the device
not ready.

At run time the driver only consumes `NAV-PVT`; it maps `fixType` 2 or 3
with `gnssFixOK` to `GNSS_FIX_STATUS_GNSS_FIX` / `GNSS_FIX_QUALITY_GNSS_SPS`,
scales latitude and longitude to nanodegrees, `headMot` to millidegrees,
passes `gSpeed` and `hMSL` through, and reports `pDOP` ×10 as `hdop`. The
pull API polls `CFG-RATE`, `CFG-NAV5` (`dynModel` 0 maps to
`GNSS_NAVIGATION_MODE_BALANCED_DYNAMICS`) and `MON-GNSS`.

## The DUT image

Two images, the same driver on either of the DUT's UARTs.
`test/dut/rp2.overlay` puts `uart0` on GP0/GP1 at `current-speed` 115200
with a `u-blox,m8` node aliased `gnss`, `initial-baudrate` 9600 and
`fix-rate` 1000 ms, and removes the console from that UART. The two rates
differ on purpose: the driver's first poll fails, and its baud negotiation
is exercised on every DUT reset. `test/dut/rp2.conf` turns the serial
driver, its interrupt-driven and runtime-configure paths and `CONFIG_GNSS`
on. `test/dut/uart1/` is the same receiver on `uart1`, GP8 (DUT TX) and GP9
(DUT RX), the straight-through pins. On the harness that pair is TX on GP9
and RX on GP8, which is no PL011's, so it is served by the PIO UART, and
the driver's baud negotiation runs on that engine:

```python
gnss = metalclaw.attach("neo_m8n", "gnss", uart=(9, 8))
```

The DUT command is `GNSS`: the last fix the driver published, in the GNSS
API's units, and how many it has published since boot. `GNSS RATE`,
`GNSS NAV` and `GNSS SYSTEMS` go through the driver's pull API, which polls
the receiver.

```sh
just build-dut neo_m8n          # or neo_m8n/uart1
METALCLAW_HARNESS_SERIAL=<serial> METALCLAW_ARM_TOOLCHAIN_DIR=<dir> \
METALCLAW_DUT_DEVICE=neo_m8n just test hardware
```

## Tests

`test/cases.py` attaches the model on the fixture's UART for the image
flashed, `(0, 1)` or `(9, 8)`, resets the DUT and checks:
the driver came up, moved the part to 115200 and left `rate_ms` 1000 and
`NAV-PVT` on with a stop and a start counted; four injected fixes across the
ranges come back through the GNSS API field for field, UTC included; a
`fix` of 0 is reported as no fix; one `NAV-PVT` goes out per interval and a
five-second jump sends one; the pull API returns the fix rate, navigation
mode and systems the model publishes; and with the part moved to 38400 from
the harness the driver does not come up, while a `reset()` back to 9600 and
a DUT reset bring it back with the fix flowing.

## Not modelled

NMEA output, `NAV-SAT` and satellite data, `CFG-VALSET`/`VALGET`, the
`navBbrMask` of `CFG-RST`, the reset pin, time pulse, and any message the
driver does not use. `valid` and `gnssFixOK` follow `fix` alone.
