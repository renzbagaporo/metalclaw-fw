<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# Devices and the runtime

A device is whatever the board under test finds on the other side of a pin,
and anything a user can name in `attach()`. Each one is a directory under
`devices/` that defines one `struct metalclaw_model` and nothing else the
runtime needs to know about. One exists: [`swd_probe`](../devices/swd_probe/README.md),
a CMSIS-DAP v2 debug probe.

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
  `swd_wire` takes a tuple `(swclk, swdio[, nreset])`; `usb_interface` takes
  no keyword, the runtime allocates it from the descriptor the model carries
  and delivers packets to the model's `usb_rx` op. Declared but not yet
  resolvable: `spi_target`, `i2c_target`, `uart`, `pwm_capture`.
- **state_size**: bytes of zeroed state the runtime hands the model.
- **ops**: `init` and `deinit` are required; `reset`, `get`, `set`,
  `inject_fault`, `clear_faults`, `read_region`, `write_region`, `prepare`,
  `time_advanced` are optional and report `ENOTSUP` when absent.

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
`metalclaw_trace()`.

`core/include/metalclaw/regmap.h` is the one response image so far: a
register file with up to four banks, per-register flags for writable,
clear-on-read and stream, and bus-side read/write helpers an engine can call
without the model.

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
- `usb.h`: a vendor bulk interface on the composite device, with a receive
  callback and a write; see [usb.md](usb.md).

## From MicroPython

```python
import metalclaw
metalclaw.models()                       # ids of every model in the build
metalclaw.schema("bmp388")               # properties, regions, faults, bindings
d = metalclaw.attach("bmp388", "baro", sda=4, scl=5)
d.get("pressure"); d.set(pressure=101325); d.pressure = 101325
d.schema(); d.faults(); d.inject("nak", 3); d.clear_faults()
d.read_region("registers", 0, 16); d.write_region("registers", 0, b"\x00")
d.trace(); d.reset(); d.detach()
metalclaw.instances(); metalclaw.detach("baro")
metalclaw.now_us(); metalclaw.advance_us(1000)
```

Property names are attributes on the instance. Unknown names raise
`ValueError`, out-of-range values raise `ValueError`, and platform refusals
raise `OSError` with the errno.
