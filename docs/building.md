<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# Building

```sh
just setup      # submodules, pinned ARM toolchain into ../.arm-none-eabi
just build      # build/rp2/<board>/firmware.uf2 and firmware.elf
```

The firmware is upstream MicroPython v1.28.0's `ports/rp2`, built unpatched
with `BOARD_DIR` pointing at a Metalclaw board and `USER_C_MODULES` pointing at
`core/micropython/micropython.cmake`. That file adds `core/` as a
subdirectory, which builds the harness core (runtime, image helpers, the
generated registry, every device) as one static library, `metalclaw_core`,
whose only include directory is `core/include`. A device or core source that
includes an SDK, port or MicroPython header fails to compile. The same
library builds on the host with nothing else:

```sh
cmake -S core -B build/core_host -G Ninja && cmake --build build/core_host
```

The usermod file then includes the port selected by `METALCLAW_PORT`
(default `rp2`), `core/ports/<port>/port.cmake`, which adds the HAL
implementation and the MicroPython glue for that chip to the firmware target.

## Boards

`METALCLAW_BOARD_DIR` selects the board directory and may point anywhere. The
in-tree default is `core/ports/rp2/boards/rpi_pi_pico`. A board is a
MicroPython board directory whose `mpconfigboard.h` sets its identity and then
includes `metalclaw_port.h`.

`core/ports/rp2/boards/metalclaw_pico_rp2` is the MetalClaw Pico RP2 v1.r1
carrier, an RP2354B with 2 MB of flash for a Pico or Pico 2 DUT; pico-sdk
has no header for it, so the board directory carries its own. GPIO47 is the
PSRAM's chip select and is reserved; every other GPIO reaches the DUT, so the
board has no LED and `machine.I2C`, `SPI` and `UART` take their pins as
arguments.

```sh
METALCLAW_BOARD_DIR=$PWD/core/ports/rp2/boards/metalclaw_pico_rp2 just build
```

## Warnings

Nothing may warn. MicroPython and pico-sdk already build with `-Wall -Werror`.
On top of that, `core/cmake/strict.cmake` defines two sets:

- `METALCLAW_STRICT_C_OPTIONS`, applied per source file to Metalclaw's own
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
