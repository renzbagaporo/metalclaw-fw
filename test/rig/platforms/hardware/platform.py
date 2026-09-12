# SPDX-License-Identifier: Apache-2.0 OR MIT
# The hardware platform: a real harness on USB and a real DUT behind its
# probe. It refuses to run without explicit identities and never scans.

import os
import subprocess
import time
from pathlib import Path

import pytest

from test.rig.platforms.hardware.mailbox import Mailbox
from test.rig.platforms.hardware.probe import ProbeRs
from test.rig.platforms.hardware.repl import Repl

ROOT = Path(__file__).resolve().parents[4]


def _required(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        pytest.exit(f"{name} is not set; the hardware platform never guesses a fixture", returncode=2)
    return value


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

    def ping(self, timeout: float = 10.0) -> dict:
        self.mailbox.wait_ready(timeout)
        return self.mailbox.ping(timeout)

    def command(self, line: str, timeout: float = 10.0) -> dict:
        return self.mailbox.command(line, timeout)


@pytest.fixture(scope="session")
def harness() -> Harness:
    serial = _required("METALCLAW_HARNESS_SERIAL")
    h = Harness(serial)
    h.wait_enumerated()
    return h


@pytest.fixture(scope="session")
def probe(harness: Harness):
    # The probe device is attached from the harness's REPL; adding its USB
    # interface re-enumerates the harness once.
    if "CMSIS-DAP v2" not in harness.usb_interfaces():
        harness.run("import metalclaw\nmetalclaw.attach('swd_probe', 'dut', swd=(2, 3, 28))")
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
