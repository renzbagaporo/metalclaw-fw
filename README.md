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
whose attributes come from each model's own descriptors. There are no device
models in the tree yet, so today it looks like this:

```python
>>> import metalclaw
>>> metalclaw.models()
()
>>> metalclaw.advance_us(1000), metalclaw.now_us()
(1000, 1000)
```

See [docs/devices.md](docs/devices.md) for the model contract a device is
written against.

## Layout

```text
core/                         the harness firmware
  include/metalclaw/          the ABI devices are written against, and the HAL
  src/                        runtime, regmap
  cmake/                      warning sets, device registry generation
  micropython/                the `metalclaw` module, Instance type, REPL stream
  ports/rp2/                  port config, USB device, HAL, board definitions
  third_party/micropython/    MicroPython v1.28.0, pinned, never patched in place
devices/                      one directory per device model (none yet)
tools/                        flash_rp2.sh: J-Link flash and recovery
docs/                         devices.md, usb.md, building.md
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
