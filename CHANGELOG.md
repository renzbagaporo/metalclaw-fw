<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# Changelog

Each release's harness firmware is on its GitHub release as a UF2 and a raw
binary, with their SHA-256 sums. Versions follow semantic versioning; before
1.0, a minor version may change the `metalclaw` module's API and the device
ABI (`MCLW_ABI_VERSION`).

## Unreleased

### Devices

- `pwm_capture`: the period, high time and duty of a PWM the DUT drives.
- `logic_capture`: the edges of a DUT output, counted, with the last
  level of each kind timed.
- `sn754410`: a TI SN754410 quadruple half-H driver, each pair's drive
  read from the DUT's PWM on its inputs.

### Harness

- The `edges_in` wire engine: every high and low on a pin the DUT drives,
  timed to a couple of harness cycles (rp2), or to the DUT's edge times
  (sim); `mclw_hal_wire_level()`, and the core's `edges.h` that the three
  models above share.
- The host bridge takes eight UART ports, eight SPI buses and eight I2C
  buses from the emulator, up from four of each.
- The DUT test app's `PWM <name> <period_ns> <pulse_ns>` command, for the
  `pwms` an image declares under `/zephyr,user`, and a `line` GPIO.

## 0.1.0

The first release.

### Harness

- MicroPython v1.28.0 for the Raspberry Pi Pico 1 (`rpi_pi_pico`) and the
  MetalClaw Pico RP2 carrier's RP2354B (`metalclaw_pico_rp2`), enumerating
  as `2FE3:0204 Metalclaw` with a CDC REPL and an MSC volume, and staying
  enumerated across soft resets. The firmware reports its version in
  `metalclaw.version()`, in the probe's CMSIS-DAP firmware string, and as
  the USB device's `bcdDevice`.
- The `metalclaw` module: a model registry, instances, a simulation clock
  that runs with real time or steps by hand, faults and trace.
- Engines in the rp2 port that serve every transfer with no CPU in the byte
  path: SPI target (command images, register maps, streams), I²C target and
  controller, UART on the PL011s and on PIO (breaks both ways, in order
  with the bytes), SPI sink, GPIO links and square waves, and the wire
  engines for timed protocols (WS2812B, APA102, HUB75, 1-Wire, DHT22,
  quadrature, NEC IR).
- The SWD debug probe: a CMSIS-DAP v2 interface that `probe-rs` drives.
- The console bridge: up to four USB serial functions carrying the board
  under test's UARTs, line coding and breaks included.

### Devices

64 device packages, 86 models, each written from its datasheet: sensors,
IMUs, ADCs, DACs, clocks, memories, displays and LED drivers on I²C, SPI and
UART, LoRa transceivers (SX1261/2/8, LLCC68), GNSS receivers, cellular
modems (SIM7000, SIM7080G, A7672x, Sequans Monarch) with a small fake
network, wire-timed parts, and pin helpers (square wave, jumpers, SPI and
I²C page links).

### Testing

- The rig (`test/rig`): every case in the tree on a bench (`hardware`), or
  on `sim`, the harness built for Linux serving a DUT emulated in Renode in
  lockstep with its virtual time.
- A Zephyr DUT image per device with Zephyr's own driver for the part, and
  87 upstream Zephyr samples that drive the models, judged by their own
  sample.yaml tests.
- Unit tests of the rig, the models and the engines; the engines' own cases
  against a stimulus in Renode; probe telemetry and soaks.
- CI on GitHub: every pull request runs the units its change reaches, every
  push to master runs all of them, and a version tag builds the release.

### Known limits

- The SPI target engine cannot serve a W25Q status read (05h) at full
  speed: its command image arms about 3 µs after chip select rises, and
  the data program uses the whole PIO instruction memory.
- The WS2812B model does not check the low times (T0L, T1L): its capture
  program has no room for them.
- An RP2040 harness serves a RAW SPI transfer that clocks its command and
  data in one go only up to about 1 MHz.
