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

A MicroPython v1.28.0 image for the Pico 1 with a `metalclaw` C module that
reports its version:

```python
>>> import metalclaw
>>> metalclaw.version()
'0.0.0'
```

## Layout

```text
core/                         the harness firmware
  micropython/                the `metalclaw` user C module
  ports/rp2/boards/rpi_pi_pico/ the Raspberry Pi Pico board definition
  third_party/micropython/    MicroPython v1.28.0, pinned, never patched in place
justfile                      setup and build recipes
```

## Building

```sh
just setup      # submodules, pinned ARM toolchain into ../.arm-none-eabi
just build      # build/rp2/firmware.uf2
```

`METALCLAW_ARM_TOOLCHAIN_DIR` and `METALCLAW_BUILD_DIR` override the toolchain
and build locations.

## License

MetalClaw is dual-licensed under your choice of either the
[Apache License 2.0](LICENSE-APACHE) or the [MIT License](LICENSE-MIT).

SPDX-License-Identifier: `Apache-2.0 OR MIT`
