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
The `metalclaw` C module only reports its version so far:

```python
>>> import metalclaw
>>> metalclaw.version()
'0.0.0'
```

## Layout

```text
core/                         the harness firmware
  cmake/                      the strict warning set for MetalClaw sources
  micropython/                the `metalclaw` user C module and the REPL stream
  ports/rp2/                  port config, USB device, board definitions
  third_party/micropython/    MicroPython v1.28.0, pinned, never patched in place
tools/                        flash_rp2.sh: J-Link flash and recovery
docs/                         usb.md, building.md
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
