<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# Testing

Tools do; tests judge. `test/rig/` is the rig: it picks a platform, finds
every case, and runs them with pytest. Cases never know which platform they
are on; they get a `harness` and a `dut` from the rig and speak to them.

```sh
just setup-rig                        # .venv with pytest and pyserial
just setup-dut                        # Zephyr workspace in ../.zephyr-workspace
just build-dut                        # build/dut/smoke/zephyr/zephyr.elf
just build-dut bmp388                 # build/dut/bmp388/, with Zephyr's bmp388 driver
just build-dut bmp388/spi             # build/dut/bmp388/spi/, the SPI variant of that image
METALCLAW_HARNESS_SERIAL=<serial> just test hardware
```

Extra arguments after `hardware` go to pytest. The rig finds the DUT
mailbox's symbols with the ARM toolchain's `nm`, from the same
`METALCLAW_ARM_TOOLCHAIN_DIR` the firmware build uses. `METALCLAW_PROBE_RS`
names the probe-rs executable when it is not on `PATH`.

## Where cases live

- `test/cases/` for the harness as a whole.
- `devices/<name>/test/cases.py` (or `cases/`) for one device.

A case is a pytest function taking `harness`, `dut`, `probe` or `fixture`:

```python
def test_dut_answers_ping(dut):
    assert dut.ping()["status"] == "ok"
```

## Platforms

Only `hardware` exists. It refuses to run without `METALCLAW_HARNESS_SERIAL`
and never scans for ports. Bringing it up means: wait for the harness to
enumerate, attach the `swd_probe` device from the REPL if the CMSIS-DAP
interface is not already present, flash the DUT image through that probe with
`probe-rs download --verify`, reset it, and wait for the mailbox to answer
PING. If any of that fails, no case runs.

`harness.run(code)` executes Python on the harness's raw REPL and returns
what it printed. `dut.command(line)` sends one mailbox command and returns the
parsed result; `dut.reset()` resets the DUT through the probe;
`dut.probe.read_words(addr, n)` reads DUT memory without halting it.

`fixture` is the platform's description of the wiring: which pin carries
which bus role, and where each part sits on the shared buses. A case builds
its attach line from it and carries no pin number of its own. On the hardware
platform it is

```python
{"swd": (2, 3, 28),
 "i2c": {"sda": 4, "scl": 5, "address": {"bmp388": 0x76}},
 "uart": {"tx": 0, "rx": 1},
 "spi": {"sck": 18, "mosi": 19, "miso": 16,
         "cs": {"ads7883": 21, "icm4268x": 17, "bmp388": 22}}}
```

and the platform attaches its own probe on `fixture["swd"]`.

`METALCLAW_DUT_DEVICE` picks which device's DUT image to flash, default
`smoke`; `METALCLAW_DUT_IMAGE` names an image path outright. Every image
reports its name in `ID` and `PING`, `dut.image_name()` returns it, and a
device's cases call `dut.require_image(name)` so they skip rather than fail
when another image is flashed; one rig run therefore exercises one device.

The harness fixture detaches every instance except the probe when the
session starts: instances outlive a run, and a leftover device would hold
pins a case needs. The simulation clock runs by default; a case that counts
what a part sends per interval pauses it and steps it with `advance_us()`,
as `neo_m8n`'s do.

## The DUT fixture

`test/rig/dut/` is a Zephyr application for the Pico 2
(`rpi_pico2/rp2350a/m33`). It boots, publishes a banner line, and serves
one-line commands through a RAM mailbox the host reads and writes over SWD
without halting the core. The record layout, the protocol and the commands
are in [test/rig/dut/README.md](../test/rig/dut/README.md). The commands are
per Zephyr subsystem, not per part: `SENSOR <alias> <channel>...` fetches
through the sensor API, `SPI <alias> [hz] [bytes] [count] [gap_us]` clocks
master windows, `RAW` clocks one window of given bytes and `GNSS` reads the
GNSS API; what a value means is the case's business. A device package
adds a devicetree overlay and a Kconfig fragment under
`devices/<name>/test/dut/rp2.{overlay,conf}` so that Zephyr's real driver for
that part exercises the simulated one; `just build-dut <name>` applies them and
builds `build/dut/<name>/`. A variant lives one level down,
`test/dut/<variant>/rp2.{overlay,conf}`, is built with
`just build-dut <name>/<variant>` and names itself `<name>/<variant>`.
Images exist for `bmp388` (I2C) and `bmp388/spi`, `ads7883`, `icm4268x` and
`neo_m8n`.

The overlays follow the `fixture` description above: the SPI bus is
GP18/GP19/GP16 with one GPIO chip select per part (the PL022's own chip
select drops whenever its transmit FIFO drains), I2C is GP4/GP5, and the
crossed UART pair is GP0/GP1. The rig's burst cases leave 20 µs between
windows, above the engine's re-arm time.

The workspace is created from `test/rig/dut/west.yml`, which pins Zephyr
v4.4.0 and the five modules the board needs. `METALCLAW_ZEPHYR_WORKSPACE`
moves it; `METALCLAW_ZEPHYR_REFERENCE` names local clones to borrow objects
from so nothing is downloaded twice. The Zephyr SDK is expected at
`ZEPHYR_SDK_INSTALL_DIR`, default `~/zephyr-sdk-1.0.1`.

## probe-rs

The installed probe-rs 0.30 has no `download --reset`, so flashing and
resetting are two sessions. `--connect-under-reset` is refused in the rig's
probe wrapper. The DUT is described to probe-rs as `RP2350_Core0`, a
core-0-only description shipped in `test/rig/platforms/hardware/`; probe-rs
picks the RP2350 debug sequence by variant name prefix, so the name matters.

`METALCLAW_PROBE_DOWNLOAD_TIMEOUT_S` sets the host deadline in seconds for each
`probe-rs download`, including verification. It defaults to 180 and must be a
positive finite number. An explicit `ProbeRs.download(timeout=...)` argument
takes precedence. An emulated fixture whose verified transfer exceeds the
default can opt into a longer deadline:

```sh
METALCLAW_PROBE_DOWNLOAD_TIMEOUT_S=900 just test hardware
```

This setting applies only to downloading the DUT image.
