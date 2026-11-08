<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# Devices and the runtime

A device is whatever the board under test finds on the other side of a pin,
and anything a user can name in `attach()`. Each one is a directory under
`devices/` that defines one `struct mclw_model` and nothing else the
runtime needs to know about. Eight exist:
[`swd_probe`](../devices/swd_probe/README.md), a CMSIS-DAP v2 debug probe;
[`console_bridge`](../devices/console_bridge/README.md), a USB serial port on
the board's console UART;
[`bmp388`](../devices/bmp388/README.md), a Bosch barometer on I2C or SPI;
[`ads7883`](../devices/ads7883/README.md), a Texas Instruments ADC on SPI;
[`icm4268x`](../devices/icm4268x/README.md), TDK InvenSense IMUs on SPI;
[`neo_m8n`](../devices/neo_m8n/README.md), a u-blox GNSS receiver on UART that
streams NMEA and speaks UBX;
[`sx126x`](../devices/sx126x/README.md), Semtech LoRa transceivers on
SPI with their BUSY, DIO1 and NRESET lines and a simulated air side; and
[`cellular`](../devices/cellular/README.md), cellular modems on UART with
their PWRKEY or RESET_N and a small fake network behind them, one engine
shared by four models: [`sim7080g`](../devices/cellular/sim7080g.md) and
[`sim7000`](../devices/cellular/sim7000.md), SIMCom LTE-M/NB-IoT modems;
[`a7672x`](../devices/cellular/a7672x.md), a SIMCom LTE Cat-1 modem; and
[`sequans_monarch`](../devices/cellular/sequans_monarch.md), the Sequans
Monarch LTE-M/NB-IoT modems.

A device is written from its datasheet and contains no bus code. It declares
which buses the part speaks, fills an image the datasheet describes (a
register map, a frame, or a command image), and arms the engine that serves
it; or, on UART, it parses what arrives and answers. The transports live in
the port.

## The model

`core/include/metalclaw/model.h`. A model is static descriptor tables plus
an ops table:

- **properties**: name, type (`bool`, `i32`, `u32`), element count (up to 8),
  unit, min, max, access. Values travel as `struct mclw_value`; the
  runtime range-checks them before the model sees them.
- **regions**: named byte ranges (a register file, a FIFO) with a size and
  access.
- **faults**: names the model knows how to misbehave under.
- **bindings**: what the model needs from the platform, by name and kind.
  The names are the `attach()` keywords. `gpio_in` and `gpio_out` take a pin;
  `swd_wire` takes a tuple `(swclk, swdio[, nreset])`; `i2c_target` takes a
  tuple `(sda, scl, address)`; `spi_target` takes `(sck, mosi, miso, cs)`;
  `uart` takes `(tx, rx)`; `usb_interface` takes no keyword, the runtime
  allocates it from the descriptor the model carries (a vendor interface or
  a serial port, and its interface string) and delivers what the host sends
  to the model's `usb_rx` op, and a serial port's line coding and control
  line changes to its `usb_control` op.
  An `spi_target` binding carries the part's datasheet facts (the SPI modes
  it supports, the default, bit order) and a `uart` binding its default baud
  and format; the runtime opens the engine with them and `schema()` shows
  them. A part that supports several SPI modes exposes an `spi_mode`
  property; a UART part exposes `baud`.
- **state_size**: bytes of zeroed state the runtime hands the model.
- **ops**: `init` and `deinit` are required; `reset`, `get`, `set`,
  `inject_fault`, `clear_faults`, `read_region`, `write_region`,
  `time_advanced`, `poll`, `usb_rx`, `usb_control` are optional and report
  `ENOTSUP` when absent.
- **config**: the model's own constant data, for one source that defines
  several models (`icm4268x` picks its `WHO_AM_I` from it, `cellular` its
  command tables, identity and timing). The runtime never
  reads it.

A model owns its response image in its state, arms the bound engine from
`init`, and edits the image under `mclw_hal_critical_enter()` whenever
a property changes. The engine serves the image with no model call. A
callback that only holds the state gets its instance back with
`mclw_instance_from_state()`. The runtime has already refused a
missing required binding before `init` runs.

A model declares the ABI version it targets twice: in `device.cmake`, where
the generator refuses a mismatch at configure time, and in the struct, where
`attach()` refuses it at run time.

## Registration

A device directory contains `device.cmake` that calls

```cmake
metalclaw_device(bmp388 SYMBOL mclw_model_bmp388 ABI 1 SOURCES ${CMAKE_CURRENT_LIST_DIR}/bmp388.c)
```

`core/cmake/devices.cmake` globs every such file and generates the registry
table. Nothing else in the tree is edited to add a device.

## The runtime

`core/include/metalclaw/runtime.h`. Instances live in a static arena sized
at build time (`METALCLAW_ARENA_SIZE`, default 16 KiB, up to
`METALCLAW_INSTANCES_MAX`, default 8). `attach()` resolves each binding
through the HAL, allocates state, and calls the model's `init`. `detach()`
reverses it. Every instance is detached on a MicroPython soft reset.

The simulation clock starts at zero at boot and runs with real time: the
worker advances it once a millisecond and calls every model's
`time_advanced`, so a part that talks on its own keeps its cadence with no
script involved (`neo_m8n` sends a fix every interval). Between ticks a
running clock reads as the last tick plus the real time since, so
`mclw_now_us()` and trace stamps taken from a callback are exact to the
microsecond while `time_advanced` keeps its millisecond cadence. `pause()`
stops it;
`advance_us()` then steps it by exactly the amount asked, which is how a
case counts frames deterministically; `resume()` lets it run again from
where it is. On the same millisecond tick, paused or not, the worker calls
every model's `poll`: what the part decides follows the clock in
`time_advanced`, and what it already decided to send leaves in real time
from `poll`, as a real part's queued output drains at line rate whatever the
script does with the clock (`neo_m8n` feeds an epoch's NMEA to the UART
there). Each instance carries a 16-entry trace ring that models append
to with `mclw_trace()`, stamped with simulation time; a kind is one of
`spi`, `i2c`, `uart`, `command`, `state`, `error`. Every engine's counters
reach a binding or a host through one runtime call,
`mclw_binding_stats()`, as named values.

## Where a model runs

Two cores. Core 0 runs MicroPython, bare metal, and TinyUSB. Core 1 is the
**worker**: it drains work the engines post from their interrupts, runs
every registered poll, and ticks the clock. Every engine interrupt is
enabled on core 1, so MicroPython's atomic sections never delay one.

| Model entry point | Runs on | Holds |
| --- | --- | --- |
| `get`, `set`, `reset`, regions, faults | core 0, the script's thread | the runtime lock |
| `usb_rx`, `usb_control` | core 0, the USB task | the runtime lock |
| `time_advanced`, `poll` | the worker | the runtime lock |
| `uart` `on_rx` | the worker | the runtime lock |
| `command` `on_window` | the worker | the runtime lock |
| `regmap` `on_write`, `stream` `on_frame` | an engine interrupt | the critical section |

The runtime lock (`mclw_hal_lock`) is what lets a model touch its
state from `set` and from `on_rx` without further care. Image edits from
any context go under `mclw_hal_critical_enter()`. It makes a multi-byte
update atomic with respect to interrupt-context code such as `on_write` and
to each per-byte call of the I2C engine; the I2C engine leaves it between
bytes and the SPI engine's DMA reads the live image, so a burst that
overlaps an update can mix the old image and the new one. Both are in `hal/platform.h`, with
real time (`mclw_hal_time_us`, `mclw_hal_delay_us`), the work
queue and the poll table.

## Images

The response images are what a model edits and an engine serves.

`core/include/metalclaw/regmap.h` is a register file with up to four banks,
per-register flags for writable and clear-on-read, multi-byte setters in
either byte order, and bus-side read/write helpers an engine calls without
the model. A model may install an
`on_write` hook: it runs in interrupt context after a write has been
committed, bank select included, and may only edit the image. The hook,
and whatever it calls, is defined with `MCLW_RAM_FUNC` so it runs from RAM,
as `icm4268x`'s is; the regmap's own functions already are. For SPI the
model adds a `struct mclw_regmap_framing` (in `hal/spi_target.h`) from
the datasheet: which command bit means read, which bits are the address,
whether a multi-byte access walks consecutive addresses, and whether one
dummy byte precedes read data. The same map serves I2C and SPI; `bmp388`
binds either.

`core/include/metalclaw/stream.h` is the other image: a fixed-length frame
of up to 64 bytes, double-buffered, that the part drives out (`OUT`, an ADC)
or receives (`IN`, delivered through `on_frame` after the window closes).
`mclw_stream_publish()` swaps in a new frame between windows.

`core/include/metalclaw/command.h` is for parts whose SPI windows are framed
by their first byte, the opcode, and that clock a status byte out while the
host sends the header (Semtech SX126x and its kin). The image holds the
status byte, up to eight fixed responses for read opcodes, a register space
of up to seven 256-byte pages addressed by 16 bits, and a 256-byte buffer
read from an 8-bit offset that wraps. Every window is answered from
hardware; afterwards the engine hands what the host sent to the image's
`on_window` hook on the worker, under the runtime lock, where the model
executes the command, edits the image and calls
`mclw_command_publish()`. A part like this holds a BUSY line high
until it has processed a command, and the host waits for it, which is what
makes all of that free of timing: the engine raises BUSY in hardware at
every chip-select release and the model lowers it with
`mclw_hal_spi_target_set_busy()` once its edits are published.

## The HAL

`core/include/metalclaw/hal/`. Ports implement it; nothing above it knows a
port exists.

- `platform.h`: the execution model: the critical section, the runtime
  lock, real time, the work queue and poll table of the worker, and
  `MCLW_RAM_FUNC`, which places a function on an engine interrupt's path
  in RAM.
- `pins.h`: pin ownership. Claims are per owner; a board may name pins that
  no device may ever claim with `METALCLAW_RESERVED_PINS`.
- `gpio.h`: one pin driven or sampled by a device. An input also latches a
  fall, so a pulse that comes and goes between two reads is still seen.
- `swd.h`: the SWD wire: transfers, bit sequences, pin control, clock. On
  rp2 it is a PIO program; every wait is bounded and a timeout resyncs the
  state machine.
- `i2c_target.h`: an I2C target that serves a `regmap` from hardware. The
  model arms it once and updates the image under a critical section; the
  engine answers the bus from interrupt context without the model. Counters:
  `transactions`, `bytes`.
- `spi_target.h`: an SPI target that serves a `regmap` (with its framing),
  a `stream` or a `command` image from hardware, in the mode the model
  asked for, with `set_mode` to change it. It publishes a clock ceiling per
  image (10.7 MHz, 15 MHz and 8 MHz) and counters (`transactions`, `bytes`,
  short and long windows, DMA faults, stale windows; for a command image a
  stale window is one that began while BUSY was high). On rp2 it is one PIO
  program and DMA; every window is served with no CPU in the byte path, and
  the model is called back only through `on_write` or `on_frame` at
  chip-select release, or through `on_window` on the worker. A command
  image's opcode, offset and address lookups are a chain of DMA control
  blocks that patch each other, which is what bounds its clock. Two targets
  may share one bus; each keeps its own chip select. At 125 MHz, back-to-back
  14-byte register reads stay clean down to a 12 µs gap between windows and
  start failing at about 10 µs, so drivers that read back-to-back without a
  BUSY line rely on a chip-select setup delay in their overlay.
- `uart.h`: a UART with `send()` that queues and returns, `set_baud`,
  `set_format`, and a receive callback delivered on the worker under the
  runtime lock with whatever accumulated, never per byte and never from an
  interrupt, so a model parses, touches its state and answers from inside it.
  On rp2 any pin pair works: a pair that carries a PL011's TX and RX
  functions gets that block while it is free; any other pair gets a PIO
  engine: a transmit and a receive state machine on PIO0 if it has room,
  14 instruction words shared by every PIO UART on that block, DMA both
  ways, 8 data bits, no parity, one or two stop bits, two at once at most.
  Its receive ring holds 1 KiB, about 90 ms at 115200 baud.
- `usb.h`: a function on the composite device, a vendor bulk interface or
  a CDC ACM serial port, with a receive callback that says how much it
  took, a control callback for a serial port's line coding and lines, and
  a write; see [usb.md](usb.md).
- `console.h`: the channel the harness's own REPL runs over; the port owns
  the transport.

On rp2 the worker is core 1, launched before the first `mp_init` with its
own stack and never reset; a soft reset leaves it running. It parks itself
for MicroPython's flash writes through the SDK's lockout, so a flash write
delays engine interrupts for its duration, as it always did on one core.

## From MicroPython

```python
import metalclaw
metalclaw.models()                       # ids of every model in the build
metalclaw.schema("bmp388")               # properties, regions, faults, bindings
d = metalclaw.attach("bmp388", "baro", i2c=(4, 5, 0x76))
d = metalclaw.attach("bmp388", "baro", spi=(18, 19, 16, 22))
g = metalclaw.attach("neo_m8n", "gnss", uart=(0, 1))
d.get("pressure"); d.set(pressure=101325); d.pressure = 101325
d.stats("spi"); g.stats("uart")          # the engine's counters, by binding name
d.schema(); d.reset(); d.detach()
d.trace()                                # (t_us, kind, addr, len, result) tuples; kind is a name
metalclaw.instances(); metalclaw.detach("baro")
metalclaw.now_us(); metalclaw.running()          # the clock runs from boot
metalclaw.pause(); metalclaw.advance_us(1000); metalclaw.resume()

d.faults(); d.inject(name, 3); d.clear_faults()   # a model that declares faults
d.read_region(name, 0, 16); d.write_region(name, 0, b"\x00")   # ... or regions
```

Property names are attributes on the instance. Unknown names raise
`ValueError`, out-of-range values raise `ValueError`, and platform refusals
raise `OSError` with the errno. `sx126x` is the one device that declares
faults and regions: its regions carry the frames that go to and come from
the air, and its faults corrupt the next injected frames. Every other
device answers `ENOTSUP` to both sets of calls.
