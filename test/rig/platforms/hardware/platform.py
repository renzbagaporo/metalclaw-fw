# SPDX-License-Identifier: Apache-2.0 OR MIT
# The hardware platform: a real harness on USB and a real DUT behind its
# probe. It refuses to run without explicit identities and never scans.

import errno
import glob
import math
import os
import time
from dataclasses import dataclass
from pathlib import Path

import pytest
import usb.core
import usb.util

from test.rig.platforms.hardware import dap
from test.rig.platforms.hardware.dap import DapMemory
from test.rig.platforms.hardware.mailbox import Mailbox, MailboxError
from test.rig.platforms.hardware.probe import ProbeRs
from test.rig.platforms.hardware.repl import Repl

ROOT = Path(__file__).resolve().parents[4]
DUT_MAILBOX_TIMEOUT_S = 10.0
DUT_MAILBOX_TIMEOUT_ENV = "METALCLAW_DUT_MAILBOX_TIMEOUT_S"
_DEFAULT_MAILBOX_TIMEOUT = object()

FIXTURE = {
    "swd": (2, 3, 28),
    "i2c": {"sda": 4, "scl": 5, "address": {"bmp388": 0x76}},
    "uart": {"tx": 0, "rx": 1},
    "uart1": {"tx": 9, "rx": 8},
    "spi": {
        "sck": 18,
        "mosi": 19,
        "miso": 16,
        "cs": {"ads7883": 21, "icm4268x": 17, "bmp388": 22, "sx126x": 21},
    },
    # GP6 is also the modem's control line: PWRKEY or RESET_N, as the
    # cellular model names it.
    "gpio": {"sx126x": {"busy": 10, "dio1": 20, "nreset": 6}, "pwrkey": 6, "reset": 6},
}

# The MetalClaw Pico RP2 carrier (v1.r1 schematic) with a Pico on it: the
# same Pico pins as fixture 1, reached through the carrier's GPIO branches.
# Where a Pico pin has two branches, each role takes the one with the pin
# function it needs; the other branch stays an input. SPI is GPIO32-39,
# so the SPI engine's PIO block sits at GPIO base 16, and the sx126x's
# BUSY, which that block drives, takes Pico GP10 through GPIO22.
METALCLAW_PICO_RP2_FIXTURE = {
    "swd": (17, 25, 9),
    "i2c": {"sda": 12, "scl": 13, "address": {"bmp388": 0x76}},
    "uart": {"tx": 2, "rx": 3},
    "uart1": {"tx": 18, "rx": 19},
    "spi": {
        "sck": 38,
        "mosi": 32,
        "miso": 35,
        "cs": {"ads7883": 41, "icm4268x": 37, "bmp388": 33, "sx126x": 41},
    },
    "gpio": {"sx126x": {"busy": 22, "dio1": 40, "nreset": 14}, "pwrkey": 14, "reset": 14},
    # Every Pico GPIO net has a 5.1 kOhm pull-up to the Pico's 3.3 V, so a
    # line nothing drives reads high, the Pico's reset pull-downs included.
    "pulls": "up",
    # Levels the harness holds while the part on a line is absent, where
    # the pull-up would make it look active: an unattached sx126x is not
    # busy, or the DUT's driver waits for it from boot.
    "idle": {22: 0},
}

# The fixture by the harness's MicroPython board.
FIXTURES = {
    "fixture1_pico": FIXTURE,
    "rpi_pi_pico": FIXTURE,
    "metalclaw_pico_rp2": METALCLAW_PICO_RP2_FIXTURE,
}


@dataclass(frozen=True)
class DutChip:
    probe_chip: str
    chip_description: Path | None
    debug_port: dap.DebugPort


# By the DUT image's CONFIG_SOC_SERIES.
DUT_CHIPS = {
    "rp2350": DutChip("RP2350_Core0", ROOT / "test/rig/platforms/hardware/rp2350_core0.yaml", dap.RP2350),
    # probe-rs's own RP2040 description selects each core's multidrop DP.
    "rp2040": DutChip("RP2040", None, dap.RP2040),
}


def dut_chip(image: Path) -> DutChip:
    config = image.parent / ".config"
    series = None
    if config.is_file():
        for line in config.read_text().splitlines():
            if line.startswith("CONFIG_SOC_SERIES="):
                series = line.split("=", 1)[1].strip('"')
    if series not in DUT_CHIPS:
        pytest.exit(f"{config} names no DUT chip the rig knows (CONFIG_SOC_SERIES={series!r})", returncode=2)
    return DUT_CHIPS[series]


DETACH_ALL_BUT_PROBE = """import metalclaw
for i in metalclaw.instances():
    if i.model != 'swd_probe':
        kinds = [b['kind'] for b in i.schema()['bindings'].values()]
        i.detach()
        if 'usb_interface' in kinds:
            print('usb')
"""


def _required(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        pytest.exit(f"{name} is not set; the hardware platform never guesses a fixture", returncode=2)
    return value


# The DUT image is built per device: `just build-dut bmp388` puts Zephyr's
# bmp388 driver in it. METALCLAW_DUT_DEVICE picks that image; the default is
# the smoke image with no device.
def dut_image() -> Path:
    device = os.environ.get("METALCLAW_DUT_DEVICE", "smoke")
    image = Path(os.environ.get("METALCLAW_DUT_IMAGE", ROOT / f"build/dut/{device}/zephyr/zephyr.elf"))
    if not image.is_file():
        pytest.exit(f"no DUT image at {image}; run `just build-dut {'' if device == 'smoke' else device}` or set METALCLAW_DUT_IMAGE", returncode=2)
    return image


def _mailbox_timeout(value):
    if value is _DEFAULT_MAILBOX_TIMEOUT:
        value = os.environ.get(DUT_MAILBOX_TIMEOUT_ENV, DUT_MAILBOX_TIMEOUT_S)
    invalid_timeout = f"invalid DUT mailbox timeout {value!r}; expected positive finite seconds"
    try:
        timeout = float(value)
    except (TypeError, ValueError, OverflowError) as error:
        raise MailboxError(invalid_timeout) from error
    if not math.isfinite(timeout) or timeout <= 0:
        raise MailboxError(invalid_timeout)
    return timeout


# A re-enumerated harness gets a new tty node, which the by-id link then
# names; the held descriptor still points at the old one.
def _port_alive(port) -> bool:
    try:
        node = os.stat(port.port)
        held = os.fstat(port.fileno())
        port.reset_input_buffer()
    except Exception:
        return False
    return (node.st_ino, node.st_rdev) == (held.st_ino, held.st_rdev)


# MicroPython's ticks_us wraps at this period.
TICKS_PERIOD_US = 1 << 30


class Harness:
    def __init__(self, serial: str):
        self.serial = serial
        self.probe_id = f"2fe3:0204:{serial}"
        self._ticks = None
        self._elapsed_us = 0
        self._held = None

    def repl(self) -> Repl:
        return Repl(self.serial)

    def board(self) -> str:
        return self.run("import sys\nprint(sys.implementation._build)").strip()

    # One raw REPL is held for the session: entering it costs about a quarter
    # second on the bench, longer than the short waits cases time. A harness
    # that re-enumerated left the held port dead; that is found before the
    # code goes out, so code is never sent twice.
    def run(self, code: str, timeout: float = 5.0) -> str:
        for attempt in range(2):
            if self._held is None:
                self._held = self.repl().__enter__()
            if not _port_alive(self._held.port):
                self._drop()
                if attempt:
                    raise RuntimeError(f"harness {self.serial} REPL port is gone")
                continue
            try:
                return self._held.run(code, timeout)
            except Exception:
                self._drop()
                raise

    def _drop(self) -> None:
        held, self._held = self._held, None
        try:
            held.__exit__(None, None, None)
        except Exception:
            pass

    # Seconds on the harness's own clock: real time on the bench, emulated
    # time on an emulated bench. A case that waits for the bench to act, or
    # times what it did, uses this clock so it means the same on both.
    # Intervals between reads must stay under the ticks period.
    def clock(self) -> float:
        ticks = int(self.run("import time\nprint(time.ticks_us())\n").strip())
        if self._ticks is not None:
            self._elapsed_us += (ticks - self._ticks) % TICKS_PERIOD_US
        self._ticks = ticks
        return self._elapsed_us / 1e6

    # The wait happens on the harness in one REPL call: each call costs a
    # round trip of about a quarter second on the bench, so timing it from
    # here would overshoot a short wait several times over.
    def sleep(self, seconds: float) -> None:
        self.run(f"import time\ntime.sleep_us({round(seconds * 1e6)})\n", seconds + 5.0)

    # By serial: another harness on the same machine has the same VID:PID.
    # A device that leaves mid-read is re-enumerating, so it has no
    # interfaces yet.
    def usb_interfaces(self) -> list[str]:
        device = usb.core.find(idVendor=dap.VENDOR_ID, idProduct=dap.PRODUCT_ID, serial_number=self.serial)
        if device is None:
            return []
        try:
            return [usb.util.get_string(device, interface.iInterface)
                    for interface in device.get_active_configuration() if interface.iInterface]
        except usb.core.USBError as error:
            if error.errno != errno.ENODEV:
                raise
            return []
        finally:
            usb.util.dispose_resources(device)

    # A serial function the harness serves for an attached device, by its
    # interface string. Linux names the tty after its interface number; the
    # string is read back from sysfs so no case assumes the layout.
    def serial_port(self, interface: str, timeout: float = 15.0) -> str:
        deadline = time.monotonic() + timeout
        while True:
            for path in sorted(glob.glob(f"/dev/serial/by-id/usb-Metalclaw_*_{self.serial}-if*")):
                tty = os.path.basename(os.path.realpath(path))
                label = Path(f"/sys/class/tty/{tty}/device/interface")
                if label.is_file() and label.read_text().strip() == interface:
                    return path
            if time.monotonic() >= deadline:
                raise TimeoutError(f"harness {self.serial} serves no serial port {interface!r}")
            time.sleep(0.25)

    def wait_enumerated(self, timeout: float = 15.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if Path(f"/dev/serial/by-id").is_dir() and any(self.serial in p.name for p in Path("/dev/serial/by-id").iterdir()):
                time.sleep(1.0)
                return
            time.sleep(0.25)
        raise TimeoutError(f"harness {self.serial} did not enumerate")


class Dut:
    def __init__(self, harness: Harness, image: Path, toolchain_dir: str):
        self.harness = harness
        self.image = image
        # probe-rs flashes and resets; the mailbox goes through DapMemory,
        # because every probe-rs session halts the DUT (see dap.py).
        chip = dut_chip(image)
        self.probe = ProbeRs(harness.probe_id, chip.chip_description, chip=chip.probe_chip)
        self.memory = DapMemory(harness.serial, chip.debug_port)
        self.mailbox = Mailbox(self.memory, image, toolchain_dir)

    # probe-rs 0.30 has no `download --reset`; flashing and resetting are two
    # sessions. A reset restarts the DUT's result sequence, so the mailbox
    # baseline must forget the old one.
    def flash(self) -> None:
        self.probe.download(self.image, reset=False)
        self.reset()

    def reset(self) -> None:
        self.probe.reset()
        self.mailbox.last_result = 0

    def ping(self, timeout=_DEFAULT_MAILBOX_TIMEOUT) -> dict:
        timeout = _mailbox_timeout(timeout)
        self.mailbox.wait_ready(timeout)
        return self.mailbox.ping(timeout)

    def command(self, line: str, timeout=_DEFAULT_MAILBOX_TIMEOUT) -> dict:
        return self.mailbox.command(line, _mailbox_timeout(timeout))

    # `start` a command and collect it with `finish`: for a command that
    # blocks the DUT while the harness has to act, such as a receive.
    def start(self, line: str, timeout: float = 10.0) -> int:
        return self.mailbox.send(line, timeout)

    def finish(self, sequence: int, timeout: float = 10.0) -> dict:
        return self.mailbox.receive(sequence, timeout)

    # The device the flashed image was built for, "smoke" for none.
    def image_name(self) -> str:
        return self.command("ID")["image"]

    def require_image(self, name: str) -> None:
        if self.image_name() != name:
            pytest.skip(f"the DUT image is not built for {name}")


@pytest.fixture(scope="session")
def fixture(harness: Harness) -> dict:
    board = harness.board()
    if board not in FIXTURES:
        pytest.exit(f"harness board {board!r} has no fixture description", returncode=2)
    return FIXTURES[board]


@pytest.fixture(scope="session")
def harness() -> Harness:
    serial = _required("METALCLAW_HARNESS_SERIAL")
    h = Harness(serial)
    h.wait_enumerated()
    # Instances outlive a rig run; a leftover device would hold pins a case
    # needs. The probe stays, detaching it would re-enumerate the harness;
    # detaching another USB-facing device does, and is waited out.
    out = h.run(DETACH_ALL_BUT_PROBE)
    if "usb" in out.split():
        time.sleep(1.5)
        h.wait_enumerated()
    return h


@pytest.fixture(scope="session")
def probe(harness: Harness, fixture: dict):
    # The probe device is attached from the harness's REPL; adding its USB
    # interface re-enumerates the harness once.
    if "CMSIS-DAP v2" not in harness.usb_interfaces():
        harness.run(f"import metalclaw\nmetalclaw.attach('swd_probe', 'dut', swd={fixture['swd']})")
        time.sleep(1.5)
        harness.wait_enumerated()
    return harness.probe_id


@pytest.fixture(scope="session")
def dut(harness: Harness, probe, fixture: dict) -> Dut:
    d = Dut(harness, dut_image(), _required("METALCLAW_ARM_TOOLCHAIN_DIR"))
    idle = fixture.get("idle", {})
    if idle:
        harness.run("from machine import Pin\n" + "".join(f"Pin({p}, Pin.OUT, value={v})\n" for p, v in idle.items()))
    d.flash()
    d.ping()
    return d
