<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# Testing

Tools do; tests judge. `test/rig/` is the rig: it picks a platform, finds
every case, and runs them with pytest. Cases never know which platform they
are on; they get a `harness` and a `dut` from the rig and speak to them.

```sh
just setup-rig                        # .venv with pytest, pyserial and pyusb
just setup-dut                        # Zephyr workspace in ../.zephyr-workspace
just build-dut                        # build/dut/smoke/zephyr/zephyr.elf
just build-dut bmp388                 # build/dut/bmp388/, with Zephyr's bmp388 driver
just build-dut bmp388/spi             # build/dut/bmp388/spi/, the SPI variant of that image
just build-dut cellular/sim7080g      # build/dut/cellular/sim7080g/, Zephyr's offloaded sim7080 driver
just build-dut cellular/a7672x        # modem_cellular over CMUX and PPP, Zephyr's IP stack
just build-dut cellular/sequans_monarch  # the same, the part on RESET_N
just build-dut cellular/sim7000       # no modem driver: the cases speak AT through the DUT's UART
METALCLAW_HARNESS_SERIAL=<serial> just test hardware
```

Extra arguments after `hardware` go to pytest. The rig finds the DUT
mailbox's symbols with the ARM toolchain's `nm`, from the same
`METALCLAW_ARM_TOOLCHAIN_DIR` the firmware build uses. `METALCLAW_PROBE_RS`
names the probe-rs executable when it is not on `PATH`.

## Where cases live

- `test/cases/` for the harness as a whole.
- `devices/<name>/test/cases.py`, or a `cases/` directory of `test_*.py`
  files, for one device; `devices/cellular/test/cases/` has one per model
  and a `common.py` they import relatively (`from .common import ...`).

A case is a pytest function taking `harness`, `dut`, `probe` or `fixture`:

```python
def test_dut_answers_ping(dut):
    assert dut.ping()["status"] == "ok"
```

A case that lets the bench act for a while, or times what it did, uses the
harness's clock: `harness.sleep(seconds)` and `harness.clock()`. That clock
is real time on the bench and emulated time in Renode, so the case means the
same on both; host time does not.

## Platforms

Two platforms exist: `hardware`, the bench, and `emulator`, the same bench
in Renode (see [The emulator](#the-emulator)). `hardware` refuses to run
without `METALCLAW_HARNESS_SERIAL` and never scans for ports. Bringing it up means: wait for the harness to
enumerate, attach the `swd_probe` device from the REPL if the CMSIS-DAP
interface is not already present, flash the DUT image through that probe with
`probe-rs download --verify`, reset it, and wait for the mailbox to answer
PING. If any of that fails, no case runs.

`harness.run(code)` executes Python on the harness's raw REPL and returns
what it printed. `harness.serial_port(interface)` waits for the serial port
the harness serves for an attached device and returns its
`/dev/serial/by-id/` path, matched by serial number and by the interface
string read back from sysfs. `dut.command(line)` sends one mailbox command
and returns the parsed result; a command that blocks the DUT while the
harness has to act (a radio receive waiting for a frame the case injects)
is sent with `dut.start(line)` and collected with `dut.finish(sequence)`;
`dut.reset()` resets the DUT through the probe;
`dut.memory.read_words(addr, n)` reads DUT memory without halting it.

Flashing and resetting go through probe-rs. Everything else goes through
`test/rig/platforms/hardware/dap.py`, which drives the harness's CMSIS-DAP
interface directly: a probe-rs session halts the DUT's cores while it
attaches and again when it ends, about 13 ms each on the RP2350, which is
long enough for a UART part's reply to overrun the DUT's 32-byte receive
FIFO.

`fixture` is the platform's description of the wiring: which pin carries
which bus role, and where each part sits on the shared buses. A case builds
its attach line from it and carries no pin number of its own. On the hardware
platform it is

```python
{"swd": (2, 3, 28),
 "i2c": {"sda": 4, "scl": 5, "address": {"bmp388": 0x76}},
 "uart": {"tx": 0, "rx": 1},
 "uart1": {"tx": 9, "rx": 8},
 "spi": {"sck": 18, "mosi": 19, "miso": 16,
         "cs": {"ads7883": 21, "icm4268x": 17, "bmp388": 22, "sx126x": 21}},
 "gpio": {"sx126x": {"busy": 7, "dio1": 20, "nreset": 6},
          "pwrkey": 6, "reset": 6}}
```

and the platform attaches its own probe on `fixture["swd"]`.

`METALCLAW_DUT_DEVICE` picks which device's DUT image to flash, default
`smoke`; `METALCLAW_DUT_IMAGE` names an image path outright. Every image
reports its name in `ID` and `PING`, `dut.image_name()` returns it, and a
device's cases call `dut.require_image(name)` so they skip rather than fail
when another image is flashed; one rig run therefore exercises one device.

The harness fixture detaches every instance except the probe when the
session starts: instances outlive a run, and a leftover device would hold
pins a case needs. If one of them had a USB interface, the harness
re-enumerates and the fixture waits it out. The simulation clock runs by default; a case that counts
what a part sends per interval pauses it and steps it with `advance_us()`,
as `neo_m8n`'s do.

## The emulator

`just test emulator` runs the same cases against both Picos emulated in
Renode, from the fork that carries the RP2040 and RP2350 models. The
justfile pins a commit of that fork and the portable package its release
workflow published for it, which bundles .NET.
Its bench script (`scripts/multi-node/metalclaw_fixture.resc` in that fork)
wires the two machines pin to pin. The harness's USB is exported over
USB/IP and imported into this host's kernel, so the harness, its CMSIS-DAP
probe and the DUT are driven exactly as on the bench. Every re-enumeration
unplugs it; the platform imports it again, as a cable that stays in would.
The kernel's descriptor timeouts run in host time, so it can give up on a
harness that is busy in emulated time; an import that has not enumerated
after 120 s is replugged. The harness flash boots with its filesystem
already formatted, as a bench harness's is after its first boot, at the
range the UF2 declares for it; formatting erases sectors with interrupts
off while USB is coming up.

```sh
just setup-emulator      # boot ROMs, patched probe-rs and Renode, next to the repository
just build               # build/rp2/rpi_pi_pico/firmware.uf2, the harness image
just build-dut           # the DUT image, as for the bench
just test emulator
```

The host needs `vhci_hcd` loaded and `usbip` runnable with `sudo -n`,
which imports a device into the running kernel: run it in a disposable VM
or CI runner, not on a workstation. `METALCLAW_RENODE_DIR`,
`METALCLAW_RENODE_ROMS` and `METALCLAW_HARNESS_IMAGE` override the Renode
package, the boot ROM directory and the harness UF2; point
`METALCLAW_RENODE_DIR` at a package built from a local checkout
(`./build.sh --no-gui -t`, then unpack `output/packages/`) to try a Renode
change. `METALCLAW_RENODE_QUANTUM` overrides the bench's synchronisation quantum
in seconds. The harness's serial is the emulated flash's unique ID,
`0123456789ABCDEF`.

Emulated SWD runs about 30 times slower than the bench in host time while
the target's own timing is unchanged. The platform stretches only host-side
waits: the probe-rs download and reset deadlines, every DUT mailbox deadline
(a case's or the default, multiplied by `METALCLAW_EMULATOR_TIME_SCALE`,
default 150, since cases give them in bench seconds), and three timeouts the
probe-rs patch in `test/rig/platforms/emulator/` makes configurable
(`CMSIS_DAP_USB_TIMEOUT_MS`, `FLASH_ALGORITHM_TIMEOUT_MULTIPLIER`,
`ARM_DEBUG_TIMEOUT_MULTIPLIER`; unset, probe-rs keeps its defaults). The
rig's own CMSIS-DAP client, which carries the mailbox, reads
`CMSIS_DAP_USB_TIMEOUT_MS` too, defaulting to 1000.
`just test emulator` uses that build, from `just setup-emulator-probe-rs`.

The pinned Renode models what the `smoke` image needs: both cores, PIO
and DMA on the RP2040, its USB device, and the RP2350's debug port behind
the wired SWD pins. Device images run as their models land.

`.github/workflows/emulator.yml` runs the emulator platform when started
by hand from the Actions tab or with `gh workflow run emulator.yml`. It
runs only the images the change since the last passing scheduled run can
reach, or every image when there is none, as
`test/rig/select.py` decides from the diff: a device's own files select its
images, a harness engine (`spi_target`, `i2c_target`, `uart`, `gpio`)
selects every device that declares a binding of that kind, documentation
selects nothing, and anything else selects every image. `EMULATED_IMAGES`
in the workflow limits that to the images the pinned Renode can run. A run
takes the images, or the commit to diff against, as inputs. The workflow
has no schedule; with one, a scheduled run skips itself when master has not
moved since the last scheduled run.

## The DUT fixture

`test/rig/dut/` is a Zephyr application for the Pico 2
(`rpi_pico2/rp2350a/m33`). It boots, publishes a banner line, and serves
one-line commands through a RAM mailbox the host reads and writes over SWD
without halting the core. The record layout, the protocol and the commands
are in [test/rig/dut/README.md](../test/rig/dut/README.md). The commands are
per Zephyr subsystem, not per part: `SENSOR <alias> <channel>...` fetches
through the sensor API, `SPI <alias> [hz] [bytes] [count] [gap_us]` clocks
master windows, `RAW` clocks a window of given bytes (repeated, for a burst),
`PULSE` holds a reset line active for a while, `GNSS` reads the GNSS API and
`LORA` configures, sends and receives through the LoRa API, `DNS`, `TCP`,
`UDP`, `HTTP` and `SOCK` go through the socket API, `MODEM` calls the vendor
API of Zephyr's offloaded modem driver, `NET` powers the device behind the
network interface and brings the interface up or down, `CELLULAR` reads the
cellular driver API, `UART` exchanges raw bytes with a part no Zephyr driver
speaks, and `GPIO` drives a pin the image declares; what a value means is
the case's business. A device package
adds a devicetree overlay and a Kconfig fragment under
`devices/<name>/test/dut/rp2.{overlay,conf}` so that Zephyr's real driver for
that part exercises the simulated one; `just build-dut <name>` applies them and
builds `build/dut/<name>/`. A variant lives one level down,
`test/dut/<variant>/rp2.{overlay,conf}`, is built with
`just build-dut <name>/<variant>` and names itself `<name>/<variant>`; a
package of several models keeps one variant per model, so `cellular` has
no image of its own, only `cellular/<model>`.
Images exist for `bmp388` (I2C) and `bmp388/spi`, `ads7883`, `icm4268x`,
`neo_m8n` (UART0), `neo_m8n/uart1` and `neo_m8n/nmea` (Zephyr's generic NMEA
driver on UART1), `sx126x` (Zephyr's loramac-node driver), `sx126x/native`
(its native driver) and `sx126x/raw` (a bare SPI device for byte-level
cases), `console_bridge`, whose image echoes its console UART, and
`cellular/sim7080g`, `cellular/a7672x`, `cellular/sequans_monarch` and
`cellular/sim7000`.

The overlays follow the `fixture` description above: the SPI bus is
GP18/GP19/GP16 with one GPIO chip select per part (the PL022's own chip
select drops whenever its transmit FIFO drains), I2C is GP4/GP5, the
crossed UART pair is GP0/GP1, and the DUT's UART1 is GP8/GP9, which the
harness serves as TX GP9, RX GP8. GP6 is the sx126x's NRESET and also
carries a modem's PWRKEY or RESET_N, whichever the model has (`gpio.pwrkey`
and `gpio.reset` both name it). The rig's burst cases leave 20 µs between
windows, above the engine's re-arm time.

The workspace is created from `test/rig/dut/west.yml`, which pins Zephyr
v4.4.0, the five modules the board needs and `loramac-node`, which the
`sx126x` image's driver is built from. `METALCLAW_ZEPHYR_WORKSPACE`
moves it; `METALCLAW_ZEPHYR_REFERENCE` names local clones to borrow objects
from so nothing is downloaded twice. The Zephyr SDK is expected at
`ZEPHYR_SDK_INSTALL_DIR`, default `~/zephyr-sdk-1.0.1`; `just setup-zephyr-sdk`
installs it there with only the Arm toolchain.

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
`METALCLAW_PROBE_RESET_TIMEOUT_S` does the same for each `probe-rs reset`,
defaulting to 30.

`METALCLAW_DUT_MAILBOX_TIMEOUT_S` sets the configured host per-stage mailbox
budget in seconds when `Dut.ping()` or `Dut.command()` omits its timeout. It
defaults to 10 and must be positive and finite; an explicit timeout argument
takes precedence. `Dut.ping()` gives that budget separately to its readiness
wait and its command transaction. Each command transaction includes its
preparatory reads, command write and result polling.

An emulated fixture with slower host probe operations can opt into these
budgets:

```sh
METALCLAW_PROBE_DOWNLOAD_TIMEOUT_S=900 \
METALCLAW_DUT_MAILBOX_TIMEOUT_S=120 \
just test hardware
```
