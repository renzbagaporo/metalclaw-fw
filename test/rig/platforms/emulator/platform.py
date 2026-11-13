# SPDX-License-Identifier: Apache-2.0 OR MIT
# The emulator platform: the bench in Renode. The harness's USB reaches this
# host over USB/IP, so the harness, probe and DUT are driven exactly as on the
# hardware platform; only bringing the bench up differs.

import math
import os
from pathlib import Path

import pytest

from test.rig.platforms.emulator.bench import LAYOUTS, Bench, BenchError, Layout
from test.rig.platforms.hardware.platform import (
    _DEFAULT_MAILBOX_TIMEOUT,
    ROOT,
    Dut,
    Harness,
    _mailbox_timeout,
    _required,
    board_fixture,
    detach_all_but_probe,
    dut_image,
    hold_idle,
    soc_series,
)

# The harness's MicroPython board picks the bench, as METALCLAW_BOARD_DIR
# picks the board `just build` builds.
HARNESS_BOARD_ENV = "METALCLAW_HARNESS_BOARD"
HARNESS_BOARD = "rpi_pi_pico"

# Host-side waits stretched for emulated time. Emulated SWD runs about 30x
# slower than the bench in host time; the target's own timing is unchanged.
SLOW_HOST_DEFAULTS = {
    "METALCLAW_PROBE_DOWNLOAD_TIMEOUT_S": "1800",
    "METALCLAW_PROBE_RESET_TIMEOUT_S": "600",
    "METALCLAW_PROBE_ACCESS_TIMEOUT_S": "600",
    # A single transfer block can stall past 10 s while the emulated harness
    # runs a flash routine on a loaded host.
    "CMSIS_DAP_USB_TIMEOUT_MS": "60000",
    # The last page's routine has no next page to overlap with, so its
    # whole run lands on one watchdog: 10-14 s of host time, past 30 s on a
    # loaded host. A timed-out routine is abandoned while it runs, and the
    # DUT locks up when it returns to a breakpoint with debug off.
    "FLASH_ALGORITHM_TIMEOUT_MULTIPLIER": "150",
    "ARM_DEBUG_TIMEOUT_MULTIPLIER": "30",
}
# Room for the cable to replug a few times (bench.REPLUG_AFTER_S).
ENUMERATION_TIMEOUT_S = 600.0


# Attaching a device that adds a USB function re-enumerates the harness, so
# its serial ports reappear on the same schedule as the harness itself.
class EmulatedHarness(Harness):
    def wait_enumerated(self, timeout: float = ENUMERATION_TIMEOUT_S) -> None:
        super().wait_enumerated(timeout)

    def serial_port(self, interface: str, timeout: float = ENUMERATION_TIMEOUT_S) -> str:
        return super().serial_port(interface, timeout)

    def wait_interfaces(self, settled, what: str, timeout: float = ENUMERATION_TIMEOUT_S) -> None:
        super().wait_interfaces(settled, what, timeout)

    def wait_interface(self, name: str, present: bool = True, timeout: float = ENUMERATION_TIMEOUT_S) -> None:
        super().wait_interface(name, present, timeout)

    # REPL deadlines are in bench seconds too, and the harness runs in the
    # same emulated time as the DUT.
    def run(self, code: str, timeout: float = 5.0) -> str:
        return super().run(code, timeout * _time_scale())


# Cases give DUT deadlines in bench seconds. The emulated DUT does the same
# work in the same emulated time, which passes about 80 times slower than
# host time even while the DUT idles; a boot that waits out an absent part
# takes ten minutes.
TIME_SCALE_ENV = "METALCLAW_EMULATOR_TIME_SCALE"
TIME_SCALE = 150.0


def _time_scale() -> float:
    value = os.environ.get(TIME_SCALE_ENV, TIME_SCALE)
    try:
        scale = float(value)
    except (TypeError, ValueError, OverflowError):
        scale = math.nan
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError(f"invalid {TIME_SCALE_ENV} {value!r}; expected a positive finite factor")
    return scale


class EmulatedDut(Dut):
    def __init__(self, harness: Harness, image: Path, toolchain_dir: str, scale: float):
        super().__init__(harness, image, toolchain_dir)
        self.scale = scale

    def ping(self, timeout=_DEFAULT_MAILBOX_TIMEOUT) -> dict:
        return super().ping(_mailbox_timeout(timeout) * self.scale)

    def command(self, line: str, timeout=_DEFAULT_MAILBOX_TIMEOUT) -> dict:
        return super().command(line, _mailbox_timeout(timeout) * self.scale)

    def start(self, line: str, timeout: float = 10.0) -> int:
        return super().start(line, timeout * self.scale)

    def finish(self, sequence: int, timeout: float = 10.0) -> dict:
        return super().finish(sequence, timeout * self.scale)


def harness_board() -> str:
    return os.environ.get(HARNESS_BOARD_ENV, HARNESS_BOARD)


def layout() -> Layout:
    board = harness_board()
    if board not in LAYOUTS:
        pytest.exit(f"no emulated bench for harness board {board!r}; {HARNESS_BOARD_ENV} is one of "
                    f"{', '.join(sorted(LAYOUTS))}", returncode=2)
    return LAYOUTS[board]


def _serial() -> str:
    return os.environ.get("METALCLAW_HARNESS_SERIAL", layout().serial)


@pytest.fixture(scope="session")
def fixture(harness: Harness) -> dict:
    return board_fixture(harness)


@pytest.fixture(scope="session")
def bench():
    for name, value in SLOW_HOST_DEFAULTS.items():
        os.environ.setdefault(name, value)
    bench_layout = layout()
    image = dut_image()
    series = soc_series(image)
    if series != bench_layout.dut_chip:
        pytest.exit(f"the {harness_board()} bench's DUT is an {bench_layout.dut_chip}, but {image} is built for "
                    f"{series!r}; build it with the matching METALCLAW_DUT_BOARD", returncode=2)
    b = Bench(
        renode_dir=Path(_required("METALCLAW_RENODE_DIR")),
        layout=bench_layout,
        harness_uf2=Path(os.environ.get("METALCLAW_HARNESS_IMAGE",
                                        ROOT / f"build/rp2/{harness_board()}/firmware.uf2")),
        roms=Path(_required("METALCLAW_RENODE_ROMS")),
        dut_image=image,
        work=Path(os.environ.get("METALCLAW_RENODE_WORK", ROOT / "build/emulator")),
        serial=_serial(),
    )
    try:
        b.start()
    except BenchError as error:
        b.stop()
        pytest.exit(str(error), returncode=2)
    yield b
    b.stop()


@pytest.fixture(scope="session")
def harness(bench) -> Harness:
    h = EmulatedHarness(_serial())
    h.wait_enumerated()
    detach_all_but_probe(h)
    return h


@pytest.fixture(scope="session")
def probe(harness: Harness, fixture: dict):
    if "CMSIS-DAP v2" not in harness.usb_interfaces():
        harness.run(f"import metalclaw\nmetalclaw.attach('swd_probe', 'dut', swd={fixture['swd']})")
        try:
            harness.wait_interface("CMSIS-DAP v2")
        except TimeoutError as error:
            pytest.exit(f"{error} after attaching swd_probe", returncode=2)
    return harness.probe_id


# The bench started with the image already in the DUT's flash; swd_probe's
# cases download it through the probe. The DUT booted with the bench, before
# the harness could hold any idle level, so it boots again once they are.
@pytest.fixture(scope="session")
def dut(bench: Bench, harness: Harness, probe, fixture: dict) -> Dut:
    d = EmulatedDut(harness, bench.dut_image, _required("METALCLAW_ARM_TOOLCHAIN_DIR"), _time_scale())
    hold_idle(harness, fixture)
    d.reset()
    d.ping()
    return d
