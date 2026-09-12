<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# MetalClaw

**A scriptable hardware harness for embedded development.**

MetalClaw is a small board that sits between a host PC and a target board. It
debugs the target, feeds it stimulus, simulates the peripherals the target
expects to find on its buses, and observes what the target does back.

The harness is a Raspberry Pi Pico 1 (RP2040) running MicroPython. Native C
owns everything with a deadline: USB, the bus engines, the debug probe.
MicroPython owns what changes between fixtures: which devices exist, which
pins they use, what state they start in.

## What exists today

A MicroPython v1.28.0 image for the Pico 1 that owns its USB device. It
enumerates as `2FE3:0204 MetalClaw` with a CDC REPL and an MSC volume, is
brought up once at boot, and stays enumerated across MicroPython soft resets.

The `metalclaw` module is the generic device API: a model registry, a runtime
with instances, a simulation clock, faults and trace, and one `Instance` type
whose attributes come from each model's own descriptors.

Two devices exist. The SWD debug probe: attaching it adds a CMSIS-DAP v2
interface to the USB device, and `probe-rs` then drives the board under test
through the harness. And a simulated BMP388 barometer on I²C, which Zephyr's
own BMP388 driver on the board under test reads as the real part:

```python
>>> import metalclaw
>>> metalclaw.models()
('bmp388', 'swd_probe')
>>> probe = metalclaw.attach("swd_probe", "dut", swd=(2, 3, 28))
>>> baro = metalclaw.attach("bmp388", "baro", i2c=(4, 5, 0x76))
>>> baro.pressure = 101325
>>> baro.temperature = 25000
```

```sh
$ probe-rs list
[0]: MetalClaw Pico 1 -- 2fe3:0204:E6612483CB313225 (CMSIS-DAP)
$ probe-rs info --probe 2fe3:0204:E6612483CB313225 --protocol swd
```

See [docs/devices.md](docs/devices.md) for the model contract and
[devices/swd_probe](devices/swd_probe/README.md) for the probe.

The rig in `test/rig/` flashes a Zephyr fixture image onto the board under
test through that probe, talks to it over a RAM mailbox, and runs every case
in the tree on real hardware. With the bmp388 image, the fixture's Zephyr
driver reads the simulated sensor and the cases compare what it read with
what was injected:

```sh
just build-dut bmp388
METALCLAW_DUT_DEVICE=bmp388 METALCLAW_HARNESS_SERIAL=<serial> just test hardware
```

## Layout

```text
core/                         the harness firmware
  include/metalclaw/          the ABI devices are written against, and the HAL
  src/                        runtime, regmap
  cmake/                      warning sets, device registry generation
  micropython/                the `metalclaw` module, Instance type, REPL stream
  ports/rp2/                  port config, USB device, HAL, board definitions
  third_party/micropython/    MicroPython v1.28.0, pinned, never patched in place
devices/                      one directory per device model: swd_probe, bmp388
test/
  rig/                        run.py, the hardware platform, the Zephyr DUT fixture
  cases/                      cases for the harness as a whole
tools/                        flash_rp2.sh: J-Link flash and recovery
docs/                         devices.md, testing.md, usb.md, building.md
justfile                      setup and build recipes
```

## Building and flashing

```sh
just setup
just build                                        # build/rp2/<board>/firmware.uf2
tools/flash_rp2.sh build/rp2/<board>/firmware.elf # over J-Link
```

See [docs/building.md](docs/building.md) for boards, flags, and the
bootloader route without a J-Link.

## License

MetalClaw is dual-licensed under your choice of either the
[Apache License 2.0](LICENSE-APACHE) or the [MIT License](LICENSE-MIT).

SPDX-License-Identifier: `Apache-2.0 OR MIT`
