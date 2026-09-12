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
METALCLAW_HARNESS_SERIAL=<serial> METALCLAW_ARM_TOOLCHAIN_DIR=<dir> just test hardware
```

Extra arguments after `hardware` go to pytest.

## Where cases live

- `test/cases/` for the harness as a whole.
- `devices/<name>/test/cases.py` (or `cases/`) for one device.
- `client/test/cases.py` for the client, when it exists.

A case is a pytest function taking `harness`, `dut`, or `probe`:

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

`METALCLAW_DUT_DEVICE` picks which device's DUT image to flash, default
`smoke`; `METALCLAW_DUT_IMAGE` names an image path outright.

## The DUT fixture

`test/rig/dut/` is a Zephyr application for the Pico 2
(`rpi_pico2/rp2350a/m33`). It boots, publishes a banner line, and serves
one-line commands (`PING`, `ID`) through a RAM mailbox the host reads and
writes over SWD without halting the core. The record layout and the protocol
are in [test/rig/dut/README.md](../test/rig/dut/README.md). A device package
adds a devicetree overlay and a Kconfig fragment under
`devices/<name>/test/dut/rp2.{overlay,conf}` so that Zephyr's real driver for
that part exercises the simulated one; `just build-dut <name>` applies them and
builds `build/dut/<name>/`. One exists: `bmp388`.

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
