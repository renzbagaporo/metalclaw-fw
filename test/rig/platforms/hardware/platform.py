# SPDX-License-Identifier: Apache-2.0 OR MIT
# The hardware platform: a real harness on USB and a real DUT behind its
# probe. It refuses to run without explicit identities and never scans.

import ast
import errno
import glob
import json
import math
import os
import time
from dataclasses import dataclass
from pathlib import Path

import pytest
import usb.core
import usb.util

from test.rig import probe_telemetry
from test.rig.platforms.hardware import dap
from test.rig.platforms.hardware.dap import DapMemory
from test.rig.platforms.emulator import dut_flash
from test.rig.platforms.hardware.mailbox import Mailbox, MailboxError, symbol_addresses
from test.rig.platforms.hardware.probe import ProbeError, ProbeRs
from test.rig.platforms.hardware.repl import Repl

ROOT = Path(__file__).resolve().parents[4]
DUT_MAILBOX_TIMEOUT_S = 10.0
DUT_MAILBOX_TIMEOUT_ENV = "METALCLAW_DUT_MAILBOX_TIMEOUT_S"
# Armv6-M and Armv8-M DHCSR: the debug key with C_DEBUGEN and C_HALT clear
# runs the core and turns halting debug off.
DHCSR = 0xE000EDF0
DHCSR_RESUME = 0xA05F0000
DHCSR_HALT = 0xA05F0003
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
    # Each DUT pin a harness pin reaches, and which: the Pico 2 is stacked on
    # the Pico 1 pin for pin, except the crossed UART0 pair. Harness GP2, GP3
    # and GP28 are the DUT's SWCLK, SWDIO and RUN, so the DUT's own GP2, GP3
    # and GP28 reach nothing. Checked both ways by a pin probe, 2026-11-15.
    "dut_pins": {0: (1,), 1: (0,), **{p: (p,) for p in (*range(4, 23), 26, 27)}},
    # DUT pins the harness can hold at an analog level: none, every pin is
    # wired straight to a harness pin.
    "analog_pins": (),
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
    # Each Pico pin's carrier GPIO branches, from the v1.r1 netlist (checked
    # on the bench pin by pin, 2026-11-07). Two branches are one net; GP26-28
    # pass a 330 R + 2.2 k RC filter, fit for slow or analog signals only.
    "dut_pins": {
        0: (3, 4), 1: (2, 5), 2: (6,), 3: (0, 7), 4: (11, 12), 5: (10, 13), 6: (14,), 7: (8, 15),
        8: (19, 20), 9: (18, 21), 10: (22,), 11: (16, 23), 12: (27, 28), 13: (26, 29), 14: (30,),
        15: (24, 31), 16: (35, 36), 17: (34, 37), 18: (38,), 19: (32, 39), 20: (40, 45), 21: (41, 46),
        22: (33,), 26: (42,), 27: (43,), 28: (44,),
    },
    # The RC filters on GP26-28 (two poles at about 48 kHz, 330 R into 10 nF
    # and 2.2 k into 1.5 nF) turn a harness PWM of 1 MHz into a level with a
    # few millivolts of ripple.
    "analog_pins": (26, 27, 28),
    # Levels the harness holds while the part on a line is absent, on a
    # board built with line pull-ups, where the pull-up would make the part
    # look active: an unattached sx126x is not busy, or the DUT's driver
    # waits for it from boot.
    "idle": {22: 0},
}

# The fixture's pins by the harness's MicroPython board.
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


# A pico-examples build carries its identity beside the ELF instead of a
# Zephyr .config: target, board and chip (see test/pico_examples).
EXAMPLE_METADATA = "example.json"


def example_metadata(image: Path) -> dict | None:
    path = image.parent / EXAMPLE_METADATA
    return json.loads(path.read_text()) if path.is_file() else None


def soc_series(image: Path) -> str | None:
    example = example_metadata(image)
    if example is not None:
        return example.get("chip")
    config = image.parent / ".config"
    if config.is_file():
        for line in config.read_text().splitlines():
            if line.startswith("CONFIG_SOC_SERIES="):
                return line.split("=", 1)[1].strip('"')
    return None


def dut_chip(image: Path) -> DutChip:
    series = soc_series(image)
    if series not in DUT_CHIPS:
        pytest.exit(f"{image} names no DUT chip the rig knows (chip {series!r} from its .config or {EXAMPLE_METADATA})",
                    returncode=2)
    return DUT_CHIPS[series]


DETACH_ALL_BUT_PROBE = """import metalclaw
for i in metalclaw.instances():
    if i.model != 'swd_probe':
        kinds = [b['kind'] for b in i.schema()['bindings'].values()]
        i.detach()
        if 'usb_interface' in kinds:
            print('usb')
""" + probe_telemetry.PROBE_SNAPSHOT


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
        # Why the last interface read found the harness but no strings.
        self._unreadable = None

    def repl(self) -> Repl:
        return Repl(self.serial)

    def close(self) -> None:
        if self._held is not None:
            self._drop()

    def board(self) -> str:
        return self.run("import sys\nprint(sys.implementation._build)").strip()

    # What the harness firmware was built to know about its board.
    def board_info(self) -> dict:
        return ast.literal_eval(self.run("import metalclaw\nprint(metalclaw.board())").strip())

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
    # A device that leaves mid-read, or that sysfs already lists but that
    # cannot be opened yet, is re-enumerating, so it has no interfaces yet.
    # So is one that refuses to open: udev gives the new device node its
    # permissions a moment after it appears, and until then pyusb fails the
    # open with EACCES, or reads no language IDs and so no strings.
    def usb_interfaces(self) -> list[str]:
        device = dap.find_harness(self.serial)
        if device is None:
            return []
        try:
            names = [name for _, name in dap.interface_strings(device)]
        except usb.core.USBError as error:
            if error.errno not in (errno.ENODEV, errno.EIO, errno.EACCES):
                raise
            self._unreadable = error
            return []
        except ValueError as error:
            self._unreadable = error
            return []
        finally:
            usb.util.dispose_resources(device)
        self._unreadable = None
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

    # Until `settled` holds for the interface strings, then until the REPL's
    # tty is back. A tty proves nothing on its own: in emulated time the old
    # enumeration's ttys can outlive the change. Nor does an empty list, which
    # is what a re-enumerating harness reads as.
    def wait_interfaces(self, settled, what: str, timeout: float = 15.0) -> None:
        deadline = time.monotonic() + timeout
        while not settled(self.usb_interfaces()):
            if time.monotonic() >= deadline:
                unreadable = getattr(self, "_unreadable", None)
                raise TimeoutError(f"harness {self.serial} did not {what}"
                                   + (f" (its strings were unreadable: {unreadable})" if unreadable else ""))
            time.sleep(0.25)
        self.wait_enumerated(timeout)

    # Attaching or detaching a device with a USB interface re-enumerates the
    # harness with or without that interface's string.
    def wait_interface(self, name: str, present: bool = True, timeout: float = 15.0) -> None:
        self.wait_interfaces(lambda found: bool(found) and (name in found) == present,
                             f"enumerate {'with' if present else 'without'} {name!r}", timeout)

    def wait_enumerated(self, timeout: float = 15.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if Path(f"/dev/serial/by-id").is_dir() and any(self.serial in p.name for p in Path("/dev/serial/by-id").iterdir()):
                time.sleep(1.0)
                return
            time.sleep(0.25)
        raise TimeoutError(f"harness {self.serial} did not enumerate")


# The DUT behind the harness's probe: flashed and reset by probe-rs, its
# memory read through DapMemory, because every probe-rs session halts the
# DUT (see dap.py). The recorder counts how each probe access went.
class Target:
    recorder = probe_telemetry.DISCARD

    def __init__(self, harness: Harness, image: Path, recorder=None, device=None):
        self.harness = harness
        self.image = image
        if recorder is not None:
            self.recorder = recorder
        chip = dut_chip(image)
        self.recorder.describe(dut={"chip": chip.probe_chip, "series": soc_series(image),
                                    "device": device or os.environ.get("METALCLAW_DUT_DEVICE", "smoke"),
                                    "image": _shown(image)})
        self.probe = ProbeRs(harness.probe_id, chip.chip_description, chip=chip.probe_chip,
                             on_failure=self.resume, recorder=recorder)
        self.memory = DapMemory(harness.serial, chip.debug_port, recorder=recorder)

    # probe-rs 0.30 has no `download --reset`; flashing and resetting are two
    # sessions.
    #
    # A download that fails part way can leave the flash erased with nothing
    # to boot, and every later case would wait out its deadlines on a DUT
    # that never answers. The image is downloaded again before the failure
    # is raised; if that fails too, no later case can pass, so the session
    # stops.
    # A program can leave the DUT asleep with its clocks stopped (every debug
    # access answers WAIT) or with its power manager ignoring the debugger
    # (the debug port never powers up), and no reset the probe sends reaches
    # it: pico-examples' low_power_* do both. They wake on a rising edge on
    # GP15 and stay awake while it is high, so the harness raises it and the
    # core is halted then. An RP2350 that still does not answer has its power
    # manager reset through the RP-AP, and is halted as it boots again. A
    # halted core then takes the download; on a DUT that is awake the first
    # halt lands at once.
    WAKE_PIN = 15
    WAKE_TIMEOUT_S = 3.0
    # How long a program may sleep on its own between two wakes:
    # low_power_dormant_timer is dormant for 10 s at a time.
    ASLEEP_MAX_S = 12.0

    def halt_awake(self, wake_harness_pin, timeout=None) -> None:
        timeout = self.WAKE_TIMEOUT_S if timeout is None else timeout
        if wake_harness_pin is not None:
            self.harness.run("from machine import Pin\nimport time\n"
                             f"_wake = Pin({wake_harness_pin}, Pin.OUT, value=0)\ntime.sleep_ms(5)\n_wake.value(1)\n")
        try:
            try:
                self.memory.write_when_reachable(DHCSR, [DHCSR_HALT], timeout)
            except dap.DapError:
                if self.memory.debug_port is not dap.RP2350:
                    raise
                dap.reset_power_manager(self.harness.serial)
                self.memory.write_when_reachable(DHCSR, [DHCSR_HALT], self.WAKE_TIMEOUT_S)
        finally:
            if wake_harness_pin is not None:
                self.harness.run(f"Pin({wake_harness_pin}, Pin.IN, None)\n")

    def flash(self, wake_harness_pin=None) -> None:
        self.halt_awake(wake_harness_pin)
        try:
            self.probe.download(self.image, reset=False)
        except ProbeError as error:
            try:
                self.probe.download(self.image, reset=False)
            except ProbeError as restore_error:
                self.recorder.retry(ProbeRs.LAYER, "download", False)
                pytest.exit(f"the DUT's flash may hold no image: downloading {self.image} failed ({error}), "
                            f"and so did downloading it again to restore it ({restore_error})", returncode=2)
            self.recorder.retry(ProbeRs.LAYER, "download", True)
            error.add_note(f"{self.image} was downloaded again after this failure, to restore the DUT's flash")
            self.reset()
            raise
        self.reset()

    def reset(self) -> None:
        self.probe.reset()

    # Through RUN, as a reset button does: what a program that counts
    # resets sees, where the probe's own reset is a debug one.
    def reset_by_run(self, count: int, low_s: float, gap_s: float) -> None:
        dap.run_resets(self.harness.serial, count, round(low_s * 1e6), round(gap_s * 1e6))

    # A probe-rs session halts the core while it attaches and resumes it as
    # it ends. One cut off by its deadline leaves the core halted with debug
    # on, and every later access would wait out its own deadline.
    def resume(self) -> None:
        self.memory.write_words(DHCSR, [DHCSR_RESUME])


# A Zephyr DUT image built for the rig, which answers on its RAM mailbox.
class Dut(Target):
    def __init__(self, harness: Harness, image: Path, toolchain_dir: str, recorder=None):
        super().__init__(harness, image, recorder)
        self.mailbox = Mailbox(self.memory, image, toolchain_dir, recorder)

    # A reset restarts the DUT's result sequence, so the mailbox baseline
    # forgets the old one.
    def reset(self) -> None:
        super().reset()
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


# A board's pins plus what its harness reports. On a board built with line
# pull-ups every line nothing drives reads high, the Pico's reset pull-downs
# included; elsewhere the absent parts' lines need no idle levels.
def fixture_for(pins: dict, board_info: dict) -> dict:
    line_pullups = bool(board_info["line_pullups"])
    return {**pins, "line_pullups": line_pullups, "idle": pins.get("idle", {}) if line_pullups else {}}


def board_fixture(harness: Harness) -> dict:
    board = harness.board()
    if board not in FIXTURES:
        pytest.exit(f"harness board {board!r} has no fixture description", returncode=2)
    return fixture_for(FIXTURES[board], harness.board_info())


# The idle levels to hold while a program whose parts take `dut_pins` boots.
# A level yields to every net those parts take: the program may drive such
# a net from its first instruction, as pio_hub75 drives GP10 as its E line,
# and a held level would fight it until the part is attached.
def idle_levels(fixture: dict, dut_pins=()) -> dict:
    net = {h: d for d, branches in fixture.get("dut_pins", {}).items() for h in branches}
    taken = set(dut_pins)
    return {p: v for p, v in fixture["idle"].items() if net.get(p) not in taken}


# Before the DUT boots, so its drivers never see an absent part as active.
def hold_idle(harness: Harness, fixture: dict, dut_pins=()) -> None:
    idle = idle_levels(fixture, dut_pins)
    if idle:
        harness.run("from machine import Pin\n" + "".join(f"Pin({p}, Pin.OUT, value={v})\n" for p, v in idle.items()))


@pytest.fixture(scope="session")
def fixture(harness: Harness) -> dict:
    return board_fixture(harness)


@pytest.fixture(scope="session")
def harness(pytestconfig) -> Harness:
    serial = _required("METALCLAW_HARNESS_SERIAL")
    recorder = probe_telemetry.recorder(pytestconfig)
    if recorder is not None:
        recorder.describe(bench={"serial": serial})
    h = Harness(serial)
    h.wait_enumerated()
    # Instances outlive a rig run; a leftover device would hold pins a case
    # needs. The probe stays, detaching it would re-enumerate the harness;
    # detaching another USB-facing device does, and is waited out.
    detach_all_but_probe(h, recorder)
    return h


# The harness cannot name a detached device's interface strings, but its
# re-enumeration drops them, so the strings it reports change. The same call
# reads the probe's counters as the session starts.
def detach_all_but_probe(harness: Harness, recorder=None) -> None:
    before = harness.usb_interfaces()
    snapshot, output = probe_telemetry.parse_snapshot(harness.run(DETACH_ALL_BUT_PROBE))
    if recorder is not None:
        recorder.probe_start = snapshot
    if "usb" in output.split():
        harness.wait_interfaces(lambda found: bool(found) and found != before,
                                "re-enumerate without the detached devices")


def _shown(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path)




@pytest.fixture(scope="session")
def probe(harness: Harness, fixture: dict, pytestconfig):
    # The probe device is attached from the harness's REPL; adding its USB
    # interface re-enumerates the harness once.
    if "CMSIS-DAP v2" not in harness.usb_interfaces():
        harness.run(f"import metalclaw\nmetalclaw.attach('swd_probe', 'dut', swd={fixture['swd']})")
        harness.wait_interface("CMSIS-DAP v2")
    yield harness.probe_id
    probe_telemetry.read_end(pytestconfig, harness)


# Armv6-M / Armv8-M debug registers (ARMv6-M and ARMv8-M ARM, C1.6).
DEMCR, AIRCR, VTOR = 0xE000EDFC, 0xE000ED0C, 0xE000ED08
DCRSR, DCRDR = 0xE000EDF4, 0xE000EDF8
DHCSR_S_HALT, DHCSR_S_REGRDY = 1 << 17, 1 << 16
DEMCR_VC_CORERESET = 1
AIRCR_SYSRESETREQ = 0x05FA0004
REG_SP, REG_PC, REG_XPSR = 13, 15, 16
XPSR_T = 1 << 24
START_TIMEOUT_S = 5.0


def write_core_register(link, register: int, value: int) -> None:
    link.write_words(DCRDR, [value])
    link.write_words(DCRSR, [(1 << 16) | register])
    deadline = time.monotonic() + START_TIMEOUT_S
    while not link.read_words(DHCSR, 1)[0] & DHCSR_S_REGRDY:
        if time.monotonic() >= deadline:
            raise TimeoutError(f"the DUT core never took register {register}")


# The way probe-rs and the SDK's own no_flash flow start an image that loads
# nothing into flash: the core halts at its reset vector, the segments go
# into SRAM, and it starts at the image's entry with VTOR on __vectors and SP
# from its first word.
def start_from_ram(link, elf: bytes, vectors: int) -> None:
    link.write_words(DHCSR, [DHCSR_HALT])
    link.write_words(DEMCR, [DEMCR_VC_CORERESET])
    link.write_words(AIRCR, [AIRCR_SYSRESETREQ])
    deadline = time.monotonic() + START_TIMEOUT_S
    while not link.read_words(DHCSR, 1)[0] & DHCSR_S_HALT:
        if time.monotonic() >= deadline:
            raise TimeoutError("the DUT core never halted at its reset vector")
    for physical, offset, size in dut_flash.segments(elf):
        data = elf[offset:offset + size] + bytes(-size % 4)
        link.write_words(physical, list(int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)))
    link.write_words(VTOR, [vectors])
    write_core_register(link, REG_SP, link.read_words(vectors, 1)[0])
    write_core_register(link, REG_PC, dut_flash.entry(elf) & ~1)
    write_core_register(link, REG_XPSR, XPSR_T)
    link.write_words(DEMCR, [0])
    link.write_words(DHCSR, [DHCSR_RESUME])


# An unmodified pico-examples program: no mailbox, judged by what it does on
# its pins (test/pico_examples). One that loads nothing into flash runs from
# SRAM, so a restart loads it again.
class Example(Target):
    def __init__(self, harness: Harness, image: Path, toolchain_dir: str, recorder=None):
        metadata = example_metadata(image)
        super().__init__(harness, image, recorder, f"pico-examples/{metadata['target']}")
        self.metadata = metadata
        self.target = self.metadata["target"]
        self.elf = image.read_bytes()
        self.ram_only = not any(dut_flash.in_flash(p) for p, _, _ in dut_flash.segments(self.elf))
        self.vectors = symbol_addresses(image, ("__vectors",), toolchain_dir)["__vectors"] if self.ram_only else None

    # The DUT pins the program's parts take, which no idle level may hold.
    def parts_dut_pins(self) -> set[int]:
        from test.pico_examples.parts import parts_dut_pins
        return parts_dut_pins(self.target, self.metadata.get("board"))

    def flash(self, wake_harness_pin=None) -> None:
        if not self.ram_only:
            super().flash(wake_harness_pin)

    # A program that sleeps on its own (low_power_* while their wake pin
    # dips or their timer runs) can be asleep when the reset lands; it is
    # halted once it is awake again, as before a download, and reset then.
    def restart(self) -> None:
        if self.ram_only:
            with self.memory.link() as link:
                start_from_ram(link, self.elf, self.vectors)
            return
        try:
            self.reset()
        except ProbeError:
            self.halt_awake(None, self.ASLEEP_MAX_S)
            self.reset()


def require_rig_image(image: Path) -> None:
    if example_metadata(image) is not None:
        pytest.skip("the DUT image is a pico-examples program, not a rig image")


def require_example_image(image: Path) -> None:
    if example_metadata(image) is None:
        pytest.skip("the DUT image is not a pico-examples program")


@pytest.fixture(scope="session")
def example(harness: Harness, probe, fixture: dict, pytestconfig) -> Example:
    image = dut_image()
    require_example_image(image)
    # Checked before anything is flashed: a reset alone would run it.
    from test.pico_examples.hazards import HARDWARE_HAZARDS, bench_limit
    metadata = example_metadata(image)
    target = metadata["target"]
    if target in HARDWARE_HAZARDS:
        pytest.skip(f"{target} {HARDWARE_HAZARDS[target]}; it runs only in the emulator")
    limit = bench_limit(target, harness.board(), metadata.get("board"))
    if limit is not None:
        pytest.skip(f"{target}'s {metadata.get('board')} build {limit}; this bench does not judge it")
    e = Example(harness, image, _required("METALCLAW_ARM_TOOLCHAIN_DIR"), probe_telemetry.recorder(pytestconfig))
    hold_idle(harness, fixture, e.parts_dut_pins())
    e.flash(fixture["dut_pins"][Target.WAKE_PIN][0])
    return e


# Neither bench cables the DUT's own USB to the rig host; the emulator
# imports it.
@pytest.fixture(scope="module")
def dut_usb():
    pytest.skip("the DUT's own USB reaches no rig host on either bench")


@pytest.fixture(scope="session")
def dut(harness: Harness, probe, fixture: dict, pytestconfig) -> Dut:
    require_rig_image(dut_image())
    d = Dut(harness, dut_image(), _required("METALCLAW_ARM_TOOLCHAIN_DIR"), probe_telemetry.recorder(pytestconfig))
    hold_idle(harness, fixture)
    d.flash(fixture["dut_pins"][Target.WAKE_PIN][0])
    d.ping()
    return d
