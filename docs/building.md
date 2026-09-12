<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# Building

```sh
just setup      # submodules, pinned ARM toolchain into ../.arm-none-eabi
just build      # build/rp2/<board>/firmware.uf2 and firmware.elf
```

The firmware is upstream MicroPython v1.28.0's `ports/rp2`, built unpatched
with `BOARD_DIR` pointing at a MetalClaw board and `USER_C_MODULES` pointing at
`core/micropython/micropython.cmake`. That cmake file also pulls in the rp2
port's own sources through `core/ports/rp2/port.cmake`.

## Boards

`METALCLAW_BOARD_DIR` selects the board directory and may point anywhere. The
in-tree default is `core/ports/rp2/boards/rpi_pi_pico`. A board is a
MicroPython board directory whose `mpconfigboard.h` sets its identity and then
includes `metalclaw_port.h`.

## Warnings

Nothing may warn. MicroPython and pico-sdk already build with `-Wall -Werror`.
On top of that, `core/cmake/strict.cmake` defines two sets:

- `METALCLAW_STRICT_C_OPTIONS`, applied per source file to MetalClaw's own
  code.
- `METALCLAW_THIRD_PARTY_C_OPTIONS`, applied to the whole firmware target, so
  MicroPython, pico-sdk, and TinyUSB build with it too. It is the strict set
  minus the flags their code cannot meet, each named in that file with the
  reason.

A new flag goes into the strict set first, then into the third-party set if
the third-party build still passes with it.

## Flashing

With a J-Link on the harness's own SWD pads, which never depends on the
firmware already on the board:

```sh
tools/flash_rp2.sh build/rp2/<board>/firmware.elf
```

Without one, from the MicroPython REPL: `machine.bootloader()`, then copy the
UF2 to the `RPI-RP2` drive that appears.

`METALCLAW_ARM_TOOLCHAIN_DIR` and `METALCLAW_BUILD_DIR` override the toolchain
and build locations, `METALCLAW_JOBS` the parallel job count, and
`METALCLAW_JLINK_SERIAL` selects the probe when more than one is attached.
