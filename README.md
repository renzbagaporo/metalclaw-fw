<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# metalclaw-fw

**Firmware for Metalclaw, a scriptable hardware harness for embedded development and hardware-in-the-loop testing.**

Metalclaw sits between a host PC and a device under test (**DUT**). It exposes a CMSIS-DAP v2 debug probe and console UART bridge over a single USB cable, stimulates the DUT's pins, simulates real off-chip peripherals on its buses so unmodified firmware drivers talk to them as real hardware, and captures what the DUT drives back.

- **Scriptable in MicroPython** — attach devices, assign pins, change sensor values, inject radio frames or faults, and control a deterministic simulation clock interactively from the REPL or from host test scripts.
- **Zero-CPU bus serving** — native C and PIO/DMA engines handle protocol timing (I²C, SPI, UART, SWD, 1-Wire, custom wire protocols), while MicroPython manages fixture configuration.
- **86 device models across 64 packages** — debug probe, console bridge, sensors, IMUs, GNSS receivers, LoRa transceivers, cellular modems, CAN, displays, LED drivers, memories, and signal/bus utilities.
- **Hardware and emulated targets** — targets the [metalclaw-pico-rp2](https://github.com/renzbagaporo/metalclaw-pico-rp2) carrier (RP2354B) as the reference hardware platform, also runs on plain Raspberry Pi Pico (RP2040) and Pico 2 (RP2350) boards, and builds as a Linux host process (`core/ports/host`) that bridges to an emulated DUT in Renode.

## Quickstart

Plug in a flashed harness and open its USB CDC REPL (on Linux, `/dev/serial/by-id/usb-Metalclaw_<product>_<serial>-if00`). Everything is driven through the built-in `metalclaw` module:

```python
>>> import metalclaw
>>> metalclaw.board()
{'line_pullups': False, 'serial_functions': 4, 'serial_functions_free': 4}
>>> len(metalclaw.models())
86
>>> metalclaw.schema("bmp388")          # inspect bindings, properties, regions, faults

# Attach a CMSIS-DAP v2 SWD probe and a USB-to-UART bridge for the DUT's console.
# Each adds its interface to the harness's composite USB device automatically.
>>> probe = metalclaw.attach("swd_probe", "dut", swd=(2, 3, 28))
>>> console = metalclaw.attach("console_bridge", "dut_console", uart=(0, 1))

# Attach simulated peripherals on the DUT's buses (default Pico harness pinout shown)
>>> baro = metalclaw.attach("bmp388", "baro", i2c=(4, 5, 0x76))
>>> baro.pressure = 101325              # pascals
>>> baro.temperature = 21500            # millidegrees C

>>> imu = metalclaw.attach("icm42688", "imu", spi=(18, 19, 16, 17))
>>> imu.set(accel=(2048, -2048, 1024), gyro=(164, 0, 0))

>>> gnss = metalclaw.attach("neo_m8n", "gnss", uart=(0, 1))
>>> gnss.set(latitude=473977420, longitude=-1223321500, fix=3)

>>> radio = metalclaw.attach("sx1262", "radio", spi=(18, 19, 16, 21), busy=10, dio1=20, nreset=6)
>>> radio.write_region("rx", 0, b"hello")   # put a frame on the air for the DUT

# Control the simulation clock (runs in real time by default)
>>> metalclaw.pause()
>>> metalclaw.advance_us(1_000_000)     # step deterministically by 1 s
>>> metalclaw.resume()

# Inspect bus counters, trace ring, or tear down
>>> imu.stats("spi")
>>> baro.trace()
>>> baro.detach()
```

> **Note:** On a Pico 1, UART0 on `(0, 1)` cannot be bound to both `console_bridge` and `neo_m8n` at the same time—bind them on separate pin pairs (any GPIO pair works; non-PL011 pairs fall back to the PIO UART engine).

### Using the attached SWD probe and console bridge

The harness enumerates as `2FE3:0204 Metalclaw` with a CDC REPL (`-if00`) and an MSC flash volume. Attaching `swd_probe` exposes a `CMSIS-DAP v2` bulk interface that works directly with `probe-rs`:

```sh
$ probe-rs list
[0]: Metalclaw -- 2fe3:0204:0123456789ABCDEF (CMSIS-DAP)

$ probe-rs info --probe 2fe3:0204:0123456789ABCDEF --protocol swd
$ probe-rs download --verify --probe 2fe3:0204:0123456789ABCDEF firmware.elf
```

Attaching `console_bridge` adds a CDC ACM port (`Metalclaw DUT console`) starting at `-if03` (`/dev/serial/by-id/usb-Metalclaw_<product>_<serial>-if03`). The USB device stays enumerated across MicroPython soft resets; if `main.py` re-attaches the same USB-facing devices, the harness does not re-enumerate. See [docs/usb.md](docs/usb.md) for interface layout and host notes.

## Supported devices

All **64 device packages** (**86 models**) are written strictly from part datasheets with no bus or port code; every model registers itself via `device.cmake` and is exposed automatically through `metalclaw.models()` and `metalclaw.schema()`.

| Category | Models | Buses / Interfaces |
| --- | --- | --- |
| **Debug & Console** | `swd_probe`, `console_bridge` | SWD + USB vendor (CMSIS-DAP v2), UART + USB CDC ACM |
| **Environmental & Temperature** | `bmp388`, `bmp280`, `bme280`, `lps22hb`, `mpl3115a2`, `hts221`, `mcp9808`, `sht3x`, `sht4x`, `tmp108`, `tmp112`, `tmp116`, `max6675`, `ds18b20`, `dht22` | I²C, SPI, 1-Wire, single-wire |
| **Motion & IMU** | `icm42688`, `icm42686`, `mpu6050`, `mpu9250`, `lis3dh`, `lsm6dso`, `lsm6dsl`, `lsm303dlhc_magn`, `mma8451q` | SPI, I²C |
| **Optical, Touch & Biometrics** | `veml6031`, `veml6046`, `vl53l0x`, `ft5336`, `tsop38238`, `zfm20` | I²C, UART, IR wire |
| **Power, ADC, DAC, RTC & Haptics** | `ina219`, `ina228`, `ina237`, `ina260`, `ads7883`, `ad5693r`, `mcp4728`, `ds3231`, `pcf8523`, `drv2605l` | I²C, SPI |
| **Wireless, GNSS & CAN** | `sx1262`, `sx1261`, `sx1268`, `llcc68`, `sim7080g`, `sim7000`, `a7672x`, `sequans_monarch`, `neo_m8n`, `pa1010d`, `mcp2515` | SPI, UART, I²C |
| **Displays & LED Drivers** | `ssd1306`, `ssd1309`, `st7789v`, `hd44780_pcf8574`, `aip31068`, `lcd_backpack`, `ht16k33`, `max7219`, `ws2812b`, `apa102`, `hub75`, `is31fl3194`, `is31fl3216a`, `is31fl3733`, `lp3943`, `lp5012`, `lp5562`, `lp5569`, `pca9633`, `sct2024`, `sx1509b` | I²C, SPI, UART, shift-register & custom wires |
| **Storage & Memory** | `at24c02`, `at45db041e`, `w25q16jv` | I²C, SPI |
| **Signals, Jumpers & Bus Peers** | `square_wave`, `quadrature_encoder`, `jumper`, `jumper_open_drain`, `jumper_open_drain_clock`, `jumper_open_drain_clock_edge`, `spi_page_target`, `spi_page_controller`, `i2c_page_controller` | GPIO, PWM, PIO link, SPI peer, I²C controller |

See [docs/devices.md](docs/devices.md) for the model contract, response images, HAL engines, and full Python API, and each package's `README.md` under [`devices/`](devices/) for part-specific bindings, properties, and datasheet references.

## Supported boards, building, and flashing

### Boards

The reference hardware platform is the **[metalclaw-pico-rp2](https://github.com/renzbagaporo/metalclaw-pico-rp2)** carrier board (RP2354B), though the firmware also runs on plain Raspberry Pi Pico (RP2040) and Pico 2 (RP2350) boards—or any RP2040/RP2350 board directory whose `mpconfigboard.h` includes `metalclaw_port.h` via `METALCLAW_BOARD_DIR`:

| Board | Chip | Directory | Notes |
| --- | --- | --- | --- |
| **[MetalClaw Pico RP2](https://github.com/renzbagaporo/metalclaw-pico-rp2)** *(reference)* | RP2354B | [`core/ports/rp2/boards/metalclaw_pico_rp2`](core/ports/rp2/boards/metalclaw_pico_rp2) | Reference carrier for a Pico or Pico 2 DUT (v1.r1); 2 MB flash, GPIO47 reserved for PSRAM CS, 5.1 kΩ line pull-ups (`METALCLAW_LINE_PULLUPS`, on by default) |
| **Raspberry Pi Pico** *(default build)* | RP2040 | [`core/ports/rp2/boards/rpi_pi_pico`](core/ports/rp2/boards/rpi_pi_pico) | Plain Pico 1 harness |
| **Raspberry Pi Pico 2 / custom RP2 boards** | RP2350 / RP2040 | Custom via `METALCLAW_BOARD_DIR` | Any MicroPython RP2 board directory whose `mpconfigboard.h` includes `metalclaw_port.h` |

### Requirements

- **Harness firmware:** `just`, `git`, `curl`, `make`, `cmake`, Python 3, and a host C compiler. `just setup` fetches the pinned submodules (MicroPython v1.28.0, pico-sdk 2.2.0) and the Linux x86_64 ARM GNU toolchain into `../.arm-none-eabi` (override with `METALCLAW_ARM_TOOLCHAIN_DIR` on other hosts).
- **Flashing:** none required via the RP2 USB bootloader (UF2); an external SWD debugger is optional (a `JLinkExe` helper is included at `tools/flash_rp2.sh`).
- **Hardware & simulated test suites:** `probe-rs` 0.30, Ninja, and Zephyr SDK 1.0.1 at `ZEPHYR_SDK_INSTALL_DIR` (default `~/zephyr-sdk-1.0.1`, installed by `just setup-zephyr-sdk`). Simulated runs also use Renode, which `just setup-renode` fetches at the version `test/rig/renode.toml` pins.

### Building

```sh
# 1. Fetch pinned submodules and ARM toolchain
just setup

# 2. Build for the default Raspberry Pi Pico (RP2040)
#    Output: build/rp2/rpi_pi_pico/firmware.uf2 and firmware.elf
just build

# Or build for the MetalClaw Pico RP2 v1.r1 carrier (RP2354B)
METALCLAW_BOARD_DIR=$PWD/core/ports/rp2/boards/metalclaw_pico_rp2 just build

# For a carrier revision without the 5.1 kΩ line pull-ups:
METALCLAW_LINE_PULLUPS=OFF \
    METALCLAW_BOARD_DIR=$PWD/core/ports/rp2/boards/metalclaw_pico_rp2 \
    just build
```

You can also build the Linux host harness (MicroPython's unix port with the `metalclaw` module, core, and all 86 models over `core/ports/host`) or compile `metalclaw_core` standalone on the host:

```sh
just build-host                                                     # build/host/mpy/metalclaw-host
cmake -S core -B build/core_host -G Ninja && cmake --build build/core_host
```

### Flashing

- **Bootloader (no external debugger needed):** hold BOOTSEL while powering the board—or run `import machine; machine.bootloader()` from the harness REPL—then copy `build/rp2/<board>/firmware.uf2` onto the `RPI-RP2` mass-storage drive.
- **External SWD debugger:** flash `build/rp2/<board>/firmware.elf` with your SWD debugger (e.g., `probe-rs download`), or with the included J-Link helper:
  ```sh
  tools/flash_rp2.sh build/rp2/<board>/firmware.elf
  ```

See [docs/building.md](docs/building.md) for environment overrides (`METALCLAW_ARM_TOOLCHAIN_DIR`, `METALCLAW_BUILD_DIR`, `METALCLAW_JOBS`, `METALCLAW_JLINK_SERIAL`) and compiler warning policy.

## Architecture overview

Metalclaw separates portable device logic from chip-specific bus engines and MicroPython glue:

- **Strict core/port boundary:** [`core/CMakeLists.txt`](core/CMakeLists.txt) builds the runtime, image helpers (`regmap`, `stream`, `command`, `sink`, `onewire`), generated device registry, and all devices as one static library (`metalclaw_core`) whose only include directory is `core/include/`. Neither core nor device code can include SDK, port, or MicroPython headers.
- **HAL & ports:** ports under `core/ports/` implement `core/include/metalclaw/hal/`:
  - [`core/ports/rp2/`](core/ports/rp2/) builds upstream MicroPython v1.28.0 unpatched via `BOARD_DIR` and `USER_C_MODULES`. **Core 0** runs bare-metal MicroPython (`_thread` disabled) and TinyUSB (owning the composite USB device: REPL, MSC, up to 4 serial functions, up to 2 vendor interfaces). **Core 1** runs the engine worker and handles all engine interrupts (PIO, DMA, UART, I²C) so VM atomic sections never stall bus timing.
  - [`core/ports/host/`](core/ports/host/) implements the same HAL on Linux, speaking lockstep newline-delimited JSON over a Unix socket ([`BRIDGE.md`](core/ports/host/BRIDGE.md)) to serve bus events and pin timing for an emulated DUT in Renode—or running standalone on the host clock.

## Testing

The test rig in [`test/rig/`](test/rig/) runs platform-agnostic pytest suites on two platforms—against Zephyr DUT fixture images (real in-tree drivers communicating via a RAM mailbox over SWD) and the upstream Zephyr samples that drive the models ([`test/zephyr_samples/`](test/zephyr_samples/)):

| Platform / Suite | Command | What it exercises |
| --- | --- | --- |
| **Hardware bench** | `METALCLAW_DUT_DEVICE=icm4268x METALCLAW_HARNESS_SERIAL=<serial> just test hardware` | Real harness wired to a real Pico 2 (default) or Pico 1 DUT |
| **Sim** | `just test sim` | Linux host harness (`just build-host`) bridged in lockstep to an emulated DUT in Renode |
| **Engine tests** | `just test-engine` | RP2 harness firmware alone in Renode driven bit-by-bit by a native stimulus ([`test/engine/`](test/engine/)) |
| **Unit tests** | `just test-unit` | Host-compiled C model checks, PIO simulator tests, and rig unit tests |
| **Host tests** | `just test-host` | The Linux host harness on its own, without Renode |

Example workflow for building a Zephyr DUT image and running hardware tests:

```sh
just setup-rig                                    # .venv with pytest, pyserial, pyusb
just setup-dut                                    # Zephyr v4.4.0 workspace in ../.zephyr-workspace
just build-dut icm4268x                           # build/dut/icm4268x/zephyr/zephyr.elf
METALCLAW_DUT_DEVICE=icm4268x METALCLAW_HARNESS_SERIAL=<serial> just test hardware
```

CI on GitHub runs the unit, host and engine tests and the sim platform on every pull request (the devices and samples a change reaches) and every push to master (all of them), and a `v<major>.<minor>.<patch>` tag publishes the release ([`CHANGELOG.md`](CHANGELOG.md)). See [docs/testing.md](docs/testing.md) for bench wiring, the sim platform, CI, probe telemetry, and soak tools (`test/rig/probe_soak.py`, `test/rig/probe_report.py`).

## Repository layout

```text
core/                         harness firmware core, MicroPython module, and ports
  include/metalclaw/          device model ABI, response images, and HAL headers (hal/)
  src/                        runtime, regmap, stream, command, sink, onewire
  cmake/                      strict warning flags and device registry generation
  CMakeLists.txt              builds `metalclaw_core` with `core/include` only
  micropython/                the `metalclaw` module and generic Instance type
  ports/rp2/                  RP2040/RP2350 port: TinyUSB owner, REPL, HAL engines, boards
  ports/host/                 Linux host port and Renode bridge (BRIDGE.md)
  third_party/micropython/    MicroPython v1.28.0 (pinned, never patched in place)
devices/                      64 self-contained device packages (86 models)
test/
  rig/                        pytest runner, hardware/sim platforms, Zephyr DUT fixture, unit selection
  cases/                      harness-wide test cases
  engine/                     bit-level engine stimulus tests against emulated harness
  host/                       host harness bridge tests (no Renode required)
  unit/                       host C model checks and rig unit tests
  zephyr_samples/             builds and cases for upstream Zephyr samples that drive the models
tools/                        flash_rp2.sh (optional SWD flash/recovery helper via JLinkExe)
docs/                         devices.md, building.md, usb.md, testing.md
justfile                      setup, build, and test recipes
CHANGELOG.md                  what each release brought
```

## Documentation

- [docs/devices.md](docs/devices.md) — device model ABI, runtime, response images, HAL engines, and Python API
- [docs/building.md](docs/building.md) — boards, build flags, warning rules, and flashing
- [docs/usb.md](docs/usb.md) — composite USB device identity, interfaces, serial functions, and re-enumeration
- [docs/testing.md](docs/testing.md) — test rig, hardware and sim platforms, DUT fixture, CI, and probe telemetry
- [core/ports/host/BRIDGE.md](core/ports/host/BRIDGE.md) — lockstep Unix-socket bridge protocol for the host harness
- [`devices/<name>/README.md`](devices/) — per-device bindings, properties, and datasheet notes

## License

metalclaw-fw is dual-licensed under your choice of either the [Apache License 2.0](LICENSE-APACHE) or the [MIT License](LICENSE-MIT).

SPDX-License-Identifier: `Apache-2.0 OR MIT`
