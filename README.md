<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# metalclaw-fw

**Firmware for Metalclaw, a scriptable hardware harness for embedded
development.**

Metalclaw is a small board that sits between a host PC and a target board. It
debugs the target, feeds it stimulus, simulates the peripherals the target
expects to find on its buses, and observes what the target does back.

The harness is a Raspberry Pi Pico 1 (RP2040) running MicroPython on one
core. Native C owns everything with a deadline: USB, the bus engines, the
debug probe; the engines' interrupts and their worker run on the other
core. MicroPython owns what changes between fixtures: which devices exist,
which pins they use, what state they start in.

## Firmware

A MicroPython v1.28.0 image for the Pico 1 that owns its USB device. It
enumerates as `2FE3:0204 Metalclaw` with a CDC REPL and an MSC volume, is
brought up once at boot, and stays enumerated across MicroPython soft resets.

The `metalclaw` module is the generic device API: a model registry, a runtime
with instances, a simulation clock that runs with real time unless paused,
faults and trace, and one `Instance` type whose attributes come from each
model's own descriptors.

## Devices

Seven device packages exist; `icm4268x` defines two models and `sx126x`
four, so `models()` lists eleven. The SWD debug probe: attaching it adds a CMSIS-DAP v2
interface to the USB device, and `probe-rs` then drives the board under test
through the harness. The console bridge: attaching it adds a serial port to
the USB device that carries the board under test's console UART. And five
simulated parts the board under test's own
Zephyr drivers read as the real thing: a BMP388 barometer on I²C or SPI, an
ADS7883 ADC on SPI, ICM-42688/42686 IMUs on SPI, a NEO-M8N GNSS receiver
on UART, and SX1262-family LoRa transceivers on SPI, whose transmissions the
harness captures and whose receptions it injects. A device is written from its datasheet with no bus code; the SPI,
I²C and UART engines live in the port and serve every transfer with no CPU in
the byte path.

```python
>>> import metalclaw
>>> metalclaw.models()
('ads7883', 'bmp388', 'console_bridge', 'icm42688', 'icm42686', 'neo_m8n', 'swd_probe', 'sx1262', 'sx1261', 'sx1268', 'llcc68')
>>> probe = metalclaw.attach("swd_probe", "dut", swd=(2, 3, 28))
>>> baro = metalclaw.attach("bmp388", "baro", i2c=(4, 5, 0x76))
>>> baro.pressure = 101325
>>> imu = metalclaw.attach("icm42688", "imu", spi=(18, 19, 16, 17))
>>> imu.accel = (2048, 0, 0)
>>> gnss = metalclaw.attach("neo_m8n", "gnss", uart=(0, 1))
>>> gnss.set(latitude=473977420, longitude=-1223321500, fix=3)   # in its NMEA every second from here on
>>> radio = metalclaw.attach("sx1262", "radio", spi=(18, 19, 16, 21), busy=7, dio1=20, nreset=6)
>>> radio.write_region("rx", 0, b"hello")                # a frame on the air for the DUT
>>> metalclaw.pause(); metalclaw.advance_us(1000000)   # or step the clock by hand
```

```sh
$ probe-rs list
[0]: Metalclaw -- 2fe3:0204:E6612483CB313225 (CMSIS-DAP)
$ probe-rs info --probe 2fe3:0204:E6612483CB313225 --protocol swd
```

See [docs/devices.md](docs/devices.md) for the model contract and
[devices/swd_probe](devices/swd_probe/README.md) for the probe.

## Testing on hardware

The board under test is a Raspberry Pi Pico 2 running Zephyr, wired to the
harness as [docs/testing.md](docs/testing.md) describes. The rig in
`test/rig/` flashes a Zephyr fixture image onto it through that probe, talks
to it over a RAM mailbox, and runs every case in the tree on real hardware.
Each device has an image with Zephyr's driver for that part; the cases
compare what the driver read with what was injected:

```sh
just setup-rig                                    # .venv with pytest and pyserial
just setup-dut                                    # Zephyr workspace in ../.zephyr-workspace
just build-dut icm4268x
METALCLAW_DUT_DEVICE=icm4268x METALCLAW_HARNESS_SERIAL=<serial> just test hardware
```

## Layout

```text
core/                         the harness firmware
  include/metalclaw/          the ABI devices are written against, and the HAL
  src/                        runtime, regmap, stream, command
  cmake/                      warning sets, device registry generation
  CMakeLists.txt              the core as one static library, `metalclaw_core`
  micropython/                the `metalclaw` module and Instance type
  ports/rp2/                  port config, USB device, REPL stream, HAL, board definitions
  third_party/micropython/    MicroPython v1.28.0, pinned, never patched in place
devices/                      one directory per device model: swd_probe, console_bridge, bmp388, ads7883, icm4268x, neo_m8n, sx126x
test/
  rig/                        run.py, the hardware platform, the Zephyr DUT fixture
  cases/                      cases for the harness as a whole
tools/                        flash_rp2.sh: J-Link flash and recovery
docs/                         devices.md, testing.md, usb.md, building.md
justfile                      setup and build recipes
```

## Requirements

- `just`, `git`, `curl`, `make`, `cmake`, Python 3 and a host C compiler.
  `just setup` fetches the pinned submodules and ARM toolchain.
- A J-Link to flash over SWD, optional: the bootloader route needs none.
- For the hardware tests: probe-rs 0.30, Ninja, and the Zephyr SDK 1.0.1
  at `ZEPHYR_SDK_INSTALL_DIR` (default `~/zephyr-sdk-1.0.1`).

## Building and flashing

```sh
just setup
just build                                        # build/rp2/<board>/firmware.uf2
tools/flash_rp2.sh build/rp2/<board>/firmware.elf # over J-Link
```

See [docs/building.md](docs/building.md) for boards, flags, and the
bootloader route without a J-Link.

## Documentation

[docs/](docs/README.md) covers the device model and runtime, testing, the USB
device, and building. Each device's README covers that part.

## License

metalclaw-fw is dual-licensed under your choice of either the
[Apache License 2.0](LICENSE-APACHE) or the [MIT License](LICENSE-MIT).

SPDX-License-Identifier: `Apache-2.0 OR MIT`
