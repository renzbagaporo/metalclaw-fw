# SPDX-License-Identifier: Apache-2.0 OR MIT
# The emulator platform: the bench in Renode. The harness's USB reaches this
# host over USB/IP, so the harness, probe and DUT are driven exactly as on the
# hardware platform; only bringing the bench up differs.

import math
import os
import time
from pathlib import Path

import pytest

from test.rig.platforms.emulator.bench import Bench, BenchError
from test.rig.platforms.hardware.platform import (
    _DEFAULT_MAILBOX_TIMEOUT,
    DETACH_ALL_BUT_PROBE,
    FIXTURE,
    ROOT,
    Dut,
    Harness,
    _mailbox_timeout,
    _required,
)

# The emulated W25Q's default unique ID, which MicroPython reports as the USB
# serial.
HARNESS_SERIAL = "0123456789ABCDEF"

# Host-side waits stretched for emulated time. Emulated SWD runs about 30x
# slower than the bench in host time; the target's own timing is unchanged.
SLOW_HOST_DEFAULTS = {
    "METALCLAW_PROBE_DOWNLOAD_TIMEOUT_S": "1800",
    "METALCLAW_PROBE_RESET_TIMEOUT_S": "600",
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


# Cases give DUT deadlines in bench seconds. The emulated DUT does the same
# work in the same emulated time, which passes this many times slower than
# host time; a boot that waits out an absent part takes minutes.
TIME_SCALE_ENV = "METALCLAW_EMULATOR_TIME_SCALE"
TIME_SCALE = 60.0


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


def _serial() -> str:
    return os.environ.get("METALCLAW_HARNESS_SERIAL", HARNESS_SERIAL)


@pytest.fixture(scope="session")
def fixture() -> dict:
    return FIXTURE


@pytest.fixture(scope="session")
def bench():
    for name, value in SLOW_HOST_DEFAULTS.items():
        os.environ.setdefault(name, value)
    roms = Path(_required("METALCLAW_RENODE_ROMS"))
    b = Bench(
        renode_dir=Path(_required("METALCLAW_RENODE_DIR")),
        harness_uf2=Path(os.environ.get("METALCLAW_HARNESS_IMAGE", ROOT / "build/rp2/rpi_pi_pico/firmware.uf2")),
        harness_rom=roms / "b2.elf",
        dut_rom=roms / "bootrom-combined.bin",
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
    out = h.run(DETACH_ALL_BUT_PROBE)
    if "usb" in out.split():
        time.sleep(1.5)
        h.wait_enumerated()
    return h


@pytest.fixture(scope="session")
def probe(harness: Harness, fixture: dict):
    # The old enumeration's tty can outlive the attach in emulated time, so
    # the wait is for the probe's own interface, not for any tty.
    if "CMSIS-DAP v2" not in harness.usb_interfaces():
        harness.run(f"import metalclaw\nmetalclaw.attach('swd_probe', 'dut', swd={fixture['swd']})")
        deadline = time.monotonic() + ENUMERATION_TIMEOUT_S
        while "CMSIS-DAP v2" not in harness.usb_interfaces():
            if time.monotonic() >= deadline:
                pytest.exit("the harness never enumerated CMSIS-DAP v2 after attaching swd_probe", returncode=2)
            time.sleep(1.0)
        harness.wait_enumerated()
    return harness.probe_id


@pytest.fixture(scope="session")
def dut(harness: Harness, probe) -> Dut:
    device = os.environ.get("METALCLAW_DUT_DEVICE", "smoke")
    image = Path(os.environ.get("METALCLAW_DUT_IMAGE", ROOT / f"build/dut/{device}/zephyr/zephyr.elf"))
    if not image.is_file():
        pytest.exit(f"no DUT image at {image}; run `just build-dut {'' if device == 'smoke' else device}` or set METALCLAW_DUT_IMAGE", returncode=2)
    d = EmulatedDut(harness, image, _required("METALCLAW_ARM_TOOLCHAIN_DIR"), _time_scale())
    d.flash()
    d.ping()
    return d
