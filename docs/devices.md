<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# Devices and the runtime

A device is whatever the board under test finds on the other side of a pin,
and anything a user can name in `attach()`. Each one is a directory under
`devices/` that defines one `struct metalclaw_model` and nothing else the
runtime needs to know about. There are no devices yet; this describes the
contract they are written against.

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
  The names are the `attach()` keywords. Kinds today: `gpio_in`, `gpio_out`;
  declared but not yet resolvable on any port: `spi_target`, `i2c_target`,
  `uart`, `pwm_capture`, `swd_wire`, `usb_interface`.
- **state_size**: bytes of zeroed state the runtime hands the model.
- **ops**: `init` and `deinit` are required; `reset`, `get`, `set`,
  `inject_fault`, `clear_faults`, `read_region`, `write_region`, `prepare`,
  `time_advanced` are optional and report `ENOTSUP` when absent.

A model declares the ABI version it targets; `METALCLAW_MODEL_CHECK` in the
generated registry refuses a mismatch at compile time.

## Registration

A device directory contains `device.cmake` that calls

```cmake
metalclaw_device(bmp388 SYMBOL metalclaw_model_bmp388 SOURCES ${CMAKE_CURRENT_LIST_DIR}/bmp388.c)
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
- `pins.h`: pin ownership. Claims are per owner; the board names pins that
  are never available with `METALCLAW_RESERVED_PINS`.
- `gpio.h`: one pin driven or sampled by a device.

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
