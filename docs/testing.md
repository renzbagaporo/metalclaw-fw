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
METALCLAW_DUT_BOARD=rpi_pico just build-dut   # the same images for a Pico 1 DUT
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
- `test/zephyr_samples/cases.py` for an unmodified upstream Zephyr sample
  that drives Metalclaw's own device models, judged by what its sample.yaml
  test says it prints and by what its parts saw of it. `builds.py` lists
  the samples and how each builds: the sample.yaml test's own `extra_args`
  and `extra_configs`, as twister builds it, plus devicetree overlays and
  Kconfig fragments of ours (`overlays/`) for the parts on the bench and
  the aliases the sample looks them up by. `just build-zephyr-samples BOARD
  OUT TARGETS` (`build.py`) builds them in the DUT's Zephyr workspace for a
  bench board and stages each as `OUT/images/<board>/zephyr/<sample>/` with
  an `example.json` that carries the test's harness and criteria.
  `targets.py` turns those into a Spec (`spec.py`): the yaml's console
  patterns matched line by line as twister matches them, after the boot
  banner, a shell test's commands typed at the prompt, a ztest suite's
  summary, and Zephyr's fault reports forbidden; a sample whose yaml judges
  it some other way has criteria of ours read from its source (`OWN`), and
  `OVERRIDES` holds what a sample needs beyond its yaml, each with its
  reason. `parts.py` names the Metalclaw parts each sample talks to
  (`attach.py`). Only samples that attach a part of ours are here: the
  rig judges Metalclaw, not Zephyr.

A case is a pytest function taking `harness`, `dut`, `probe` or `fixture`:

```python
def test_dut_answers_ping(dut):
    assert dut.ping()["status"] == "ok"
```

A case that lets the bench act for a while, or times what it did, uses the
harness's clock: `harness.sleep(seconds)` and `harness.clock()`. That clock
is real time on the bench and the DUT's virtual time on the sim platform, so
the case means the same on both; host time does not.

## Platforms

Two platforms exist: `hardware`, a bench, and `sim`, the DUT alone in
Renode with its parts served by Metalclaw running as a Linux process (see
[The sim platform](#the-sim-platform)). `hardware` refuses to run
without `METALCLAW_HARNESS_SERIAL` and never scans for ports. Bringing it up means: wait for the harness to
enumerate, attach the `swd_probe` device from the REPL if the CMSIS-DAP
interface is not already present, flash the DUT image through that probe with
`probe-rs download --verify`, reset it, and wait for the mailbox to answer
PING. If any of that fails, no case runs.

On both platforms the session header shows the host's load average and CPU
count, and every failure carries a `host load` section with the same and,
where the host has `/proc/stat`, how busy its CPUs were during that test and
how much time the hypervisor stole from them. A failure on a loaded host is
weaker evidence than one on an idle host.
Every session also records how the SWD probe behaved (see
[Probe telemetry](#probe-telemetry)).

`harness.run(code)` executes Python on the harness's raw REPL and returns
what it printed. `harness.serial_port(interface)` waits for the serial port
the harness serves for an attached device and returns its
`/dev/serial/by-id/` path, matched by serial number and by the interface
string read back from sysfs. Attaching or detaching a device with a USB
interface re-enumerates the harness; `harness.wait_interface(name)` waits
until it is back with that interface string, `present=False` until it is
back without it, and then for its REPL. It goes by the strings, not by a
tty, which in emulated time can outlive the change. `dut.command(line)`
sends one mailbox command and returns the parsed result; a command that blocks the DUT while the
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
its attach line from it and carries no pin number of its own. The hardware
platform picks it by the harness's MicroPython board
(`sys.implementation._build`). For a stacked Raspberry Pi Pico harness
(`rpi_pi_pico`) with the DUT stacked on it, it is

```python
{"swd": (2, 3, 28),
 "i2c": {"sda": 4, "scl": 5, "address": {"bmp388": 0x76}},
 "uart": {"tx": 0, "rx": 1},
 "uart1": {"tx": 9, "rx": 8},
 "spi": {"sck": 18, "mosi": 19, "miso": 16,
         "cs": {"ads7883": 21, "icm4268x": 17, "bmp388": 22, "sx126x": 21}},
 "gpio": {"sx126x": {"busy": 10, "dio1": 20, "nreset": 6},
          "pwrkey": 6, "reset": 6}}
```

and the platform attaches its own probe on `fixture["swd"]`.

On the MetalClaw Pico RP2 carrier (`metalclaw_pico_rp2`) the DUT's Pico
pins are the same, reached through the carrier's GPIOs. A Pico pin with two
carrier branches gets the one with the pin function its role needs; the
other stays an input:

```python
{"swd": (17, 25, 9),
 "i2c": {"sda": 12, "scl": 13, "address": {"bmp388": 0x76}},
 "uart": {"tx": 2, "rx": 3},
 "uart1": {"tx": 18, "rx": 19},
 "spi": {"sck": 38, "mosi": 32, "miso": 35,
         "cs": {"ads7883": 41, "icm4268x": 37, "bmp388": 33, "sx126x": 41}},
 "gpio": {"sx126x": {"busy": 22, "dio1": 40, "nreset": 14},
          "pwrkey": 14, "reset": 14},
 "idle": {22: 0}}
```

Both UART pairs are the RP2350's UART_AUX function on UART0, so one of them
falls back to the PIO UART when both are attached. The SPI bus is GPIO32-39,
which puts the SPI engine's PIO block at GPIO base 16; the sx126x's BUSY,
which that block drives, is Pico GP10 through GPIO22.

The pins come from the harness's board name; the rest of the fixture comes
from what the harness firmware reports in `metalclaw.board()`. On a board
built with line pull-ups (`line_pullups`, on for the carrier v1.r1, which
pulls every Pico GPIO net up with 5.1 kOhm) a line nothing drives reads
high, even through the DUT's reset. There the harness holds the `idle`
levels before it flashes the DUT, so that a part not yet attached does not
look active (an unattached sx126x would read busy and hold the DUT's driver
in its boot); elsewhere `idle` is empty. For a Zephyr sample a level yields
to every net the sample's parts take, which the sample may drive itself
from its first instruction. A case reads `fixture["line_pullups"]` where a
line's undriven level matters.

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

## The sim platform

`just test sim` runs the cases against the DUT emulated in Renode on its
own, with its parts served by the host harness: Metalclaw built for Linux
(`just build-host`), MicroPython's unix port with the `metalclaw` module,
the runtime and every device, over a software HAL in `core/ports/host`.
The models, the runtime and the REPL the cases drive are the firmware's
own; what differs from hardware is the HAL under them, which serves the
DUT's buses from the emulator instead of from PIO and DMA. The rp2 port's
engines are not exercised here; the engine tests and the two benches are
where they are judged.

Renode comes from the [RP2 Renode fork](https://github.com/renzbagaporo/renode)
that carries the RP2040 and RP2350 models, as
the portable package its release workflow published for the commit
`test/rig/renode.toml` pins, which bundles .NET; `just setup-renode` fetches
and checks it, with both chips' boot ROMs. The emulator holds only the DUT,
a Pico or a Pico 2 (`scripts/multi-node/metalclaw_pico_native.resc`,
`metalclaw_pico2_native.resc`), chosen by the image's chip. A bridge in it
hands every I2C event, SPI frame and bridged UART's byte of the DUT's
blocks, and every level it drives on a pin the harness watches, to the host
harness over a Unix socket, in lockstep with the DUT's virtual time: each
request carries that time and waits for its answer, so a run is the same
at any host load (`core/ports/host/BRIDGE.md`). The harness's clock,
`metalclaw.now_us()`, is the DUT's virtual time, and so are the cases'
bench seconds, the DUT mailbox's deadlines included: the DUT runs slower
than the host, so a deadline counts its time, and one whose clock stands
still for 120 s of wall time fails.

The platform starts the host harness on a pseudo-terminal, which the rig
opens as it opens a harness's USB serial port, and Renode with the DUT's
image in its flash. A USB serial function of the harness is a terminal
too, linked by its interface string under the run's `usb` directory, so
the DUT's console comes through `console_bridge` as on a bench: a UART on
pins no bridged block has, the console's UART0 or a PIO program's, goes
bit by bit. A jumper between two DUT pins is a join inside Renode, so
blocks on joined pins talk to each other directly. The DUT's memory, for
the rig's mailbox, comes from Renode's monitor; a reset and a halt are
monitor commands. DUT pins are the harness's pins one to one. The cases of
the harness's own USB functions (the SWD probe, the console bridge's) skip.

```sh
just build-host
just build-dut
just setup-renode
just test sim
```

`METALCLAW_RENODE_DIR` and `METALCLAW_RENODE_ROMS` override the Renode
package and the boot ROM directory; point `METALCLAW_RENODE_DIR` at a
package built from a local checkout (`./build.sh --no-gui -t`, then unpack
`output/packages/`) to try a Renode change. `METALCLAW_RENODE_INSTANCE`
lets several sessions run on one host: instance N's Renode takes monitor
port 1334 + N and console port 1434 + N. `METALCLAW_SIM_WORK` names the
directory for the bridge's socket and the session's logs, default
`build/sim`. `METALCLAW_CONSOLE_LOG` names a file a sample case appends the
DUT's console to as it arrives, which keeps what a sample printed when its
session is killed before the report.

The host harness runs on its own too, with its REPL on stdin and its clock
the host's monotonic clock, and `just test-host` (`test/host`) drives it as
the emulator would, without Renode.

## Engine tests

`just test-engine` runs `test/engine` against the harness's engines
themselves: the harness firmware alone in Renode (`scripts/multi-node/
metalclaw_harness_native.resc`, or `metalclaw_harness_rp2_native.resc` for
the carrier's RP2354B), its GPIO bridged (`core/ports/host/BRIDGE.md`, with
the harness in the DUT's place) to a native stimulus,
`test/engine/stimulus.py`. The stimulus drives the harness's pins bit by bit
as a DUT would, an I2C or SPI controller, a UART, raw pulses, at exact
virtual times, and reads what the harness drives back, so a case can try
the edges a DUT image never makes: a pulse just outside a window, a frame
cut short. The benches still judge the engines on silicon; the emulated
PIO's timing is the limit here.

The harness's USB is exported over USB/IP and imported into this host's
kernel (`test/rig/renode`), so the cases reach its REPL as on a bench. Every
re-enumeration unplugs it; the bench imports it again, as a cable that
stays in would. The kernel's descriptor timeouts run in host time, so it
can give up on a harness that is busy in emulated time; an import whose
harness has not enumerated, or whose REPL tty is gone, for 120 s is
replugged. The harness flash boots with its filesystem already formatted,
as a bench harness's is after its first boot: formatting erases sectors
with interrupts off while USB is coming up. REPL deadlines are stretched by
`METALCLAW_RENODE_TIME_SCALE`, default 150.

The host needs `vhci_hcd` loaded, `usbip` runnable with `sudo -n`, and
usb-storage told to ignore the harness (`echo 2fe3:0204:i | sudo tee
/sys/module/usb_storage/parameters/quirks` after `sudo modprobe usb-storage`),
which changes the running kernel: run it in a disposable VM or CI runner, not
on a workstation. `METALCLAW_RENODE_INSTANCE` takes the bench's monitor
port to 1234 + N and its USB/IP port to 3240 + N.

A session that ends early still stops its Renode: an interrupt or
`pytest.exit` while the bench starts or stops, SIGINT, SIGTERM and SIGHUP
(the last two as a KeyboardInterrupt, so pytest tears down), and an
interpreter exit (atexit). Signals that arrive while the bench stops wait
until Renode is down. Renode is asked to `quit`, sent SIGTERM after 30 s,
and SIGKILL 10 s later, as it can ignore SIGTERM. Started through
util-linux's `setpriv --pdeathsig KILL` where that exists, Renode also dies
with a rig killed outright.

## Unit tests

`just test-unit` runs `test/unit`: the rig's own code, and models and
engines built for this host with the firmware's warnings, behind stand-ins
for the engines they use. Some drive a model with a transcription of the
pico-examples program that uses it and judge what the program would print
(`test/unit/programs/`, from pico-examples 0d62f75); the wire engines' PIO
programs run in a PIO emulator (`pio_sim.py`) against the DUT programs'
own.

## CI

Three workflows run on GitHub's hosted runners, all through the suite in
`.github/workflows/ci.yml`: the harness firmware and the host harness
built, the unit, host and engine tests, and the rig units on the sim
platform, each in at most eight jobs named for the units they hold. `ci-ok`
stands for the whole run.

| Workflow | Runs on | Units |
| --- | --- | --- |
| `pull` (`pull/<number>`) | every push to a pull request; a newer push cancels the older run | those the change reaches |
| `master` | every push to master, and by hand | every unit |
| `release` (`release/<tag>`) | a `v<major>.<minor>.<patch>` tag | every unit, then the release |

A unit is a device package's DUT images and the upstream samples whose main
part is one of its models, or the smoke image; each is built and run for
both DUT boards (`test/rig/platforms.toml`) by `just run-units`.
`test/rig/select.py` picks the units a change reaches: a device's files
select the units that run its images or attach one of its models
(`console_bridge` carries every sample's console, so it reaches them all),
a core library (`regmap`, `stream`, `sink`, `onewire`, `command`) the
devices that include it, an engine the devices that declare a binding of
its kind, on the platform's own port only (`core/ports/host` for sim), an
overlay the samples built with it, documentation and the unit, host and
engine tests nothing, and anything else every unit. The Renode package
pinned in `test/rig/renode.toml` is one of those.

The release workflow checks that the tag names the version
`core/include/metalclaw/version.h` gives and that `CHANGELOG.md` has a
section for it, runs the suite, checks that the commit carries every status
`release.required` in `test/rig/platforms.toml` lists as passed, and
publishes the harness firmware as UF2 and as a raw binary with their
SHA-256 sums. The benches are not on GitHub: a bench runner posts its
result as a `rig/hardware/<bench>` status, and listing that status in
`release.required` makes it a release gate.

## The DUT fixture

`test/rig/dut/` is a Zephyr application for the Pico 2
(`rpi_pico2/rp2350a/m33`) or, with `METALCLAW_DUT_BOARD=rpi_pico`, the
Pico 1; `test/rig/dut/boards/rpi_pico.conf` gives the RP2040 the timer-based
random source and the 16 KiB heap the Pico 2 board brings itself. It boots, publishes a banner line, and serves
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
harness serves as TX GP9, RX GP8. GP10 is the sx126x's BUSY. GP6 is its NRESET and also
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

`probe-rs` 0.30 has no `download --reset`, so flashing and
resetting are two sessions. `--connect-under-reset` is refused in the rig's
probe wrapper. The rig reads the DUT's chip from the image's
`CONFIG_SOC_SERIES` (`zephyr/.config` next to `zephyr.elf`). An RP2350 is
described to probe-rs as `RP2350_Core0`, a core-0-only description shipped
in `test/rig/platforms/hardware/`; probe-rs picks the RP2350 debug sequence
by variant name prefix, so the name matters. An RP2040 uses probe-rs's own
`RP2040`, and `dap.py` selects its core 0 on the multidrop bus with
TARGETSEL `0x01002927`.

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
and `METALCLAW_PROBE_ACCESS_TIMEOUT_S` for each `probe-rs read` and `write`;
both default to 30.
A probe-rs session halts the DUT's core while it attaches and resumes it
as it ends, so one cut off by its deadline leaves the core halted with
halting debug on. After any failed probe-rs run the rig resumes the core
through the DAP client (DHCSR with `C_HALT` and `C_DEBUGEN` clear) before
raising the error, so the cases after it still reach the mailbox.

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

## Probe telemetry

Every rig session, on both platforms, records each probe-rs run the rig
makes (`download`, `reset`, `read`, `write`) and each access of its DAP
client (`connect`, `read`, `write`): the outcome, the duration, the words
moved, and the retries the mailbox and `Dut.flash` make, with whether a
retry recovered. A failure is counted by kind. The DAP client knows its own:
`wait`, `fault`, `no_ack` and `protocol_error` from a transfer's ACK,
`usb_timeout` and `usb_error` from pyusb, `short_reply`, `bad_reply`,
`no_harness`, `no_interface` and the like from the connect. A probe-rs
failure is a `timeout` or `not_found`, or is classed by its output
(`probe_open`, `usb`, `dap_command`, `verify`, `fault`, `wait`, `no_ack`,
`probe_timeout`, otherwise `exit`); the first 20 failure messages are kept
as they were. A call whose data a caller rejects (a probe-rs read with
output it cannot decode) moves from ok to that kind, so calls are always ok
plus errors.

All of it is counted in memory: two clock reads and a few dict updates per
access, with durations kept as a histogram of eight buckets per octave, so
a percentile is within 9 % and sessions merge. The harness probe's own
counters (`transfers`, `errors`, `short_replies`, from
[swd_probe](../devices/swd_probe/README.md)) are read as the session starts,
in the REPL call that detaches leftover devices, and once more after the
last case, in one REPL call of its own; that call is the only traffic the
telemetry adds. A harness that is gone, or a probe that is not attached,
leaves the counters out with a note saying why.

When the session ends it appends one JSON line: the platform, the bench
(harness serial and board), the DUT chip and image, start and end, the
counts and timings per operation, the probe's counters at both ends and
their difference, and pytest's exit status. The line goes to the file
`METALCLAW_PROBE_LOG` names, else to `probe_telemetry.jsonl` beside the
`--junitxml` report, else nowhere; the terminal summary says which:

```text
probe: 3 probe-rs runs, 0 failed; 1514 DAP accesses, 1 failed (usb_timeout 1), 1 retried; probe counted transfers +41230, errors +0, short_replies +0; logged to build/rig/probe_telemetry.jsonl
```


`test/rig/probe_report.py` rolls any number of these files up, given as
paths or quoted globs: per bench, and apart for rig sessions and each kind
of soak, one row per operation with its calls, errors and error rate,
retries and retry rate, how many retries recovered, the p50, p95 and p99
durations and the longest, and the errors by kind; then the harness probe's
own counters summed over the sessions that read them. `--source rig|soak`,
`--platform` and `--since <ISO time>` narrow it.

```text
$ test/rig/probe_report.py 'build/rig/*.jsonl'
hardware E6612483CB313225 fixture1_pico [rig]: 2 sessions, 2026-11-15 09:46 .. 2026-11-15 10:46 UTC, 1.9min in all
  layer     operation  calls  errors  error%  retries  retry%  recovered      p50      p95      p99      max  errors by kind
  dap       connect     3000       0  0.000%        0  0.000%          0    2.0ms    2.9ms    3.4ms    4.5ms
  dap       read        3002       2  0.067%        2  0.067%          2    4.5ms    6.9ms    8.2ms    1.00s  usb_timeout 2
  probe_rs  download       2       0  0.000%        0  0.000%          0    4.15s    4.15s    4.15s    4.15s
  probe_rs  reset          2       0  0.000%        0  0.000%          0  400.0ms  400.0ms  400.0ms  400.0ms
  harness probe: 420000 transfers, 2 wire errors (1 in 210000), 0 short replies, over 2 of 2 sessions
```

## Probe soaks

`test/rig/probe_soak.py` works one bench's SWD path hard and logs it in
the same format, for the same rollup. It needs the harness's serial, never
scans, and refuses to start on anything it cannot check.

```sh
test/rig/probe_soak.py flash --serial <serial> \
    --image build/dut/smoke/zephyr/zephyr.elf \
    --image build/dut/bmp388/zephyr/zephyr.elf \
    --cycles 1000 --log probe_soak.jsonl
test/rig/probe_soak.py memory --serial <serial> \
    --image build/dut/smoke/zephyr/zephyr.elf \
    --duration 1h --log probe_soak.jsonl
test/rig/probe_soak.py memory --jlink <J-Link serial> --harness <serial> \
    --image build/dut/smoke/zephyr/zephyr.elf \
    --duration 1h --log probe_soak.jsonl
```

`flash` runs `probe-rs download --verify` and then `probe-rs reset` through
the harness probe, alternating the two images so the flash changes every
cycle. A failed step is tried again up to `--retries` times (default 2), and
the soak stops after `--max-failed-cycles` failed cycles in a row (default
5). It refuses two images that put the same bytes in flash or are for
different chips, and when it ends, interrupted or not, it downloads the
first image again unless that was the last one flashed (`--no-restore`
leaves it).

`memory` runs for `--duration` (default 1 h) against a DUT running
`--image`: it writes a pseudo-random block (`--block-words`, default 64) to
the image's scratch RAM, reads it back and compares, and reads a window of
the image's flash (`--flash-words`, default 64) and compares it with the
ELF's bytes, going round every whole word the image puts in flash. The
scratch RAM is the image's `mclw_dut_scratch` (see
[the DUT fixture](../test/rig/dut/README.md#the-scratch-area)); without it
the soak refuses, unless `--scratch ADDRESS:BYTES` names an area in the
chip's SRAM that holds none of the image's RAM objects and is not in
picolibc's malloc arena or a MicroPython heap. `--read-only` writes nothing.
Before the first write it checks that the DUT's flash holds the image and
that the image's mailbox header is initialised, so the image is running;
`--flash` downloads the image and resets the DUT first. A halted core is
resumed, as the rig does after a cut-off probe-rs session. Every access goes
through the rig's DAP client, as the mailbox does, and is counted in the
`dap` rows; each comparison is a `soak` row whose failures are a `mismatch`
or the access's error kind. The soak stops after `--max-consecutive-errors`
iterations in a row (default 100) that cannot reach the DUT.

With `--jlink` the same memory soak goes through a J-Link instead, as a
baseline: by pylink over one connection when pylink is installed
(`--jlink-device` names the target, default `RP2350_M33_0` or
`RP2040_M0_0`), or by probe-rs (`--backend probe-rs`), whose every access
is a session that halts the core. The same checks apply, so a J-Link that
is wired to another chip than the image's is refused at the flash check,
and it never flashes: `--flash` with `--jlink` is refused, since a J-Link on
the harness's own SWD pads would download the DUT image into the harness.

Each run appends a line per flash cycle and per failed memory comparison
(up to 1000, the rest counted), then the session line, whose `soak` field
holds the mode, the arguments, the seed, the cycles or iterations, why it
stopped and whether it was interrupted; then it prints the session's
rollup. The harness probe's counters are read at both ends. Ctrl-C or
SIGTERM ends a soak early with the same line and summary, marked
interrupted. `--log` defaults to `METALCLAW_PROBE_LOG`; there is no other
default.
