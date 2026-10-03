# SPDX-License-Identifier: Apache-2.0 OR MIT
# The hardware platform: a real harness on USB and a real DUT behind its
# probe. It refuses to run without explicit identities and never scans.

import glob
import math
import os
import subprocess
import time
from pathlib import Path

import pytest

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
    "gpio": {"sx126x": {"busy": 7, "dio1": 20, "nreset": 6}},
}


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


class Harness:
    def __init__(self, serial: str):
        self.serial = serial
        self.probe_id = f"2fe3:0204:{serial}"

    def repl(self) -> Repl:
        return Repl(self.serial)

    def run(self, code: str, timeout: float = 5.0) -> str:
        with self.repl() as r:
            return r.run(code, timeout)

    def usb_interfaces(self) -> list[str]:
        out = subprocess.run(["lsusb", "-v", "-d", f"2fe3:0204"], capture_output=True, text=True).stdout
        names = []
        for line in out.splitlines():
            parts = line.split(None, 2)
            if len(parts) == 3 and parts[0] == "iInterface" and parts[1] != "0":
                names.append(parts[2].strip())
        return names

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
        self.probe = ProbeRs(harness.probe_id, ROOT / "test/rig/platforms/hardware/rp2350_core0.yaml")
        self.mailbox = Mailbox(self.probe, image, toolchain_dir)

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
def fixture() -> dict:
    return FIXTURE


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
def dut(harness: Harness, probe) -> Dut:
    # The DUT image is built per device: `just build-dut bmp388` puts Zephyr's
    # bmp388 driver in it. METALCLAW_DUT_DEVICE picks that image; the default
    # is the smoke image with no device.
    device = os.environ.get("METALCLAW_DUT_DEVICE", "smoke")
    image = Path(os.environ.get("METALCLAW_DUT_IMAGE", ROOT / f"build/dut/{device}/zephyr/zephyr.elf"))
    if not image.is_file():
        pytest.exit(f"no DUT image at {image}; run `just build-dut {'' if device == 'smoke' else device}` or set METALCLAW_DUT_IMAGE", returncode=2)
    d = Dut(harness, image, _required("METALCLAW_ARM_TOOLCHAIN_DIR"))
    d.flash()
    d.ping()
    return d
