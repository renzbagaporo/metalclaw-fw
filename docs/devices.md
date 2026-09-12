<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# Devices and the runtime

A device is whatever the board under test finds on the other side of a pin,
and anything a user can name in `attach()`. Each one is a directory under
`devices/` that defines one `struct metalclaw_model` and nothing else the
runtime needs to know about. Five exist:
[`swd_probe`](../devices/swd_probe/README.md), a CMSIS-DAP v2 debug probe;
[`bmp388`](../devices/bmp388/README.md), a Bosch barometer on I2C or SPI;
[`ads7883`](../devices/ads7883/README.md), a Texas Instruments ADC on SPI;
[`icm4268x`](../devices/icm4268x/README.md), TDK InvenSense IMUs on SPI; and
[`neo_m8n`](../devices/neo_m8n/README.md), a u-blox GNSS receiver on UART.

A device is written from its datasheet and contains no bus code. It declares
which buses the part speaks, fills an image the datasheet describes (a
register map or a frame), and arms the engine that serves it; or, on UART, it
parses what arrives and answers. The transports live in the port.

## The model

`core/include/metalclaw/model.h`. A model is static descriptor tables plus
an ops table:

- **properties**: name, type (`bool`, `i32`, `u32`), element count (up to 8),
  unit, min, max, access. Values travel as `struct metalclaw_value`; the
  runtime range-checks them before the model sees them.
- **regions**: named byte ranges (a register file, a FIFO) with a size and
  access.
- **faults**: names the model knows how to misbehave under.
- **bindings**: what the model needs from the platform, by name and kind.
  The names are the `attach()` keywords. `gpio_in` and `gpio_out` take a pin;
  `swd_wire` takes a tuple `(swclk, swdio[, nreset])`; `i2c_target` takes a
  tuple `(sda, scl, address)`; `spi_target` takes `(sck, mosi, miso, cs)`;
  `uart` takes `(tx, rx)`; `usb_interface` takes no keyword, the runtime
  allocates it from the descriptor the model carries and delivers packets to
  the model's `usb_rx` op.
  An `spi_target` binding carries the part's datasheet facts (the SPI modes
  it supports, the default, bit order) and a `uart` binding its default baud
  and format; the runtime opens the engine with them and `schema()` shows
  them. A part that supports several SPI modes exposes an `spi_mode`
  property; a UART part exposes `baud`.
- **state_size**: bytes of zeroed state the runtime hands the model.
- **ops**: `init` and `deinit` are required; `reset`, `get`, `set`,
  `inject_fault`, `clear_faults`, `read_region`, `write_region`,
  `time_advanced`, `usb_rx` are optional and report `ENOTSUP` when absent.
- **config**: the model's own constant data, for one source that defines
  several models (`icm4268x` picks its `WHO_AM_I` from it). The runtime never
  reads it.

A model owns its response image in its state, arms the bound engine from
`init`, and edits the image under `metalclaw_hal_critical_enter()` whenever
a property changes. The engine serves the image with no model call.

A model declares the ABI version it targets twice: in `device.cmake`, where
the generator refuses a mismatch at configure time, and in the struct, where
`attach()` refuses it at run time.

## Registration

A device directory contains `device.cmake` that calls

```cmake
metalclaw_device(bmp388 SYMBOL metalclaw_model_bmp388 ABI 1 SOURCES ${CMAKE_CURRENT_LIST_DIR}/bmp388.c)
```

`core/cmake/devices.cmake` globs every such file and generates the registry
table. Nothing else in the tree is edited to add a device.

## The runtime

`core/include/metalclaw/runtime.h`. Instances live in a static arena sized
at build time (`METALCLAW_ARENA_SIZE`, default 8 KiB, up to
`METALCLAW_INSTANCES_MAX`, default 8). `attach()` resolves each binding
through the HAL, allocates state, and calls the model's `init`. `detach()`
reverses it. Every instance is detached on a MicroPython soft reset.

The simulation clock starts at zero and moves only when `advance_us()` is
called, which also notifies every model through `time_advanced`. Each
instance carries a 16-entry trace ring that models append to with
`metalclaw_trace()`; a kind is one of `spi`, `i2c`, `uart`, `command`,
`state`, `error`. Every engine's counters reach a binding or a host through
one runtime call, `metalclaw_binding_stats()`, as named values.

The simulation clock is also how a part that talks on its own gets its
cadence: `neo_m8n` sends a fix from `time_advanced` when the clock reaches
the next interval.

## Images

The response images are what a model edits and an engine serves.

`core/include/metalclaw/regmap.h` is a register file with up to four banks,
per-register flags for writable and clear-on-read, and bus-side read/write
helpers an engine calls without the model. A model may install an
`on_write` hook: it runs in interrupt context after a write has been
committed, bank select included, and may only edit the image. For SPI the
model adds a `struct metalclaw_regmap_framing` (in `hal/spi_target.h`) from
the datasheet: which command bit means read, which bits are the address,
whether a multi-byte access walks consecutive addresses, and whether one
dummy byte precedes read data. The same map serves I2C and SPI; `bmp388`
binds either.

`core/include/metalclaw/stream.h` is the other image: a fixed-length frame
of up to 64 bytes, double-buffered, that the part drives out (`OUT`, an ADC)
or receives (`IN`, delivered through `on_frame` after the window closes).
`metalclaw_stream_publish()` swaps in a new frame between windows.

## The HAL

`core/include/metalclaw/hal/`. Ports implement it; nothing above it knows a
port exists.

- `platform.h`: a critical-section pair.
- `pins.h`: pin ownership. Claims are per owner; a board may name pins that
  no device may ever claim with `METALCLAW_RESERVED_PINS`.
- `gpio.h`: one pin driven or sampled by a device.
- `swd.h`: the SWD wire: transfers, bit sequences, pin control, clock. On
  rp2 it is a PIO program; every wait is bounded and a timeout resyncs the
  state machine.
- `i2c_target.h`: an I2C target that serves a `regmap` from hardware. The
  model arms it once and updates the image under a critical section; the
  engine answers the bus from interrupt context without the model. Counters:
  `transactions`, `bytes`.
- `spi_target.h`: an SPI target that serves a `regmap` (with its framing)
  or a `stream` from hardware, in the mode the model asked for, with
  `set_mode` to change it. It publishes a clock ceiling per image and
  counters (`transactions`, `bytes`, short and long windows, DMA faults,
  stale windows). On rp2 it is one PIO program and a DMA chain; every
  window is served with no CPU in the byte path, and the model is called
  back only through `on_write` or `on_frame` at chip-select release. Two
  targets may share one bus; each keeps its own chip select. Release to
  re-arm costs about 10 µs, so drivers that read back-to-back rely on a
  chip-select setup delay in their overlay.
- `uart.h`: a UART with `send()` that queues and returns, `set_baud`, and a
  receive callback delivered in thread context with whatever accumulated,
  never per byte and never from an interrupt, so a model parses and answers
  from inside it.
- `usb.h`: a vendor bulk interface on the composite device, with a receive
  callback and a write; see [usb.md](usb.md).
- `console.h`: the channel the harness's own REPL runs over; the port owns
  the transport.

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
metalclaw.now_us(); metalclaw.advance_us(1000)

d.faults(); d.inject(name, 3); d.clear_faults()   # a model that declares faults
d.read_region(name, 0, 16); d.write_region(name, 0, b"\x00")   # ... or regions
```

Property names are attributes on the instance. Unknown names raise
`ValueError`, out-of-range values raise `ValueError`, and platform refusals
raise `OSError` with the errno. No device in the tree declares a fault or a
region, so both sets of calls answer `ENOTSUP` today.
