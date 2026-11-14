# SPDX-License-Identifier: Apache-2.0 OR MIT
# The bench in Renode: the harness, the DUT and their wires in one emulation,
# with the harness's USB exported over USB/IP and imported into this host's
# kernel, so the host sees the same USB device the bench does.

import contextlib
import fcntl
import os
import socket
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path

from . import dut_flash, storage, uf2

MASK_ROM_EXTRACTOR = "scripts/multi-node/extract_rp2040_mask_rom.py"


# What differs between benches: the bench script in the Renode package, the
# chip on each side, and the DUT's machine in that script.
@dataclass(frozen=True)
class Layout:
    script: str
    harness_chip: str
    dut_chip: str
    dut_machine: str
    # The USB serial the emulated harness reports.
    serial: str
    # The bench script variable that sets the harness's chip ID, and with it
    # its USB serial, when the script has one.
    chip_id_variable: str | None = None


# By the harness's MicroPython board.
LAYOUTS = {
    # Fixture 1: a Pico harness driving a Pico 2 DUT. The RP2040 reports the
    # emulated W25Q's default unique ID.
    "rpi_pi_pico": Layout("scripts/multi-node/metalclaw_fixture.resc", "rp2040", "rp2350", "pico2",
                          "0123456789ABCDEF"),
    # The MetalClaw Pico RP2 carrier: an RP2354B harness driving a Pico DUT.
    # The RP2350 reports its OTP chip ID, which the bench script sets.
    "metalclaw_pico_rp2": Layout("scripts/multi-node/metalclaw_carrier.resc", "rp2350", "rp2040", "pico",
                                 "00000000C0FFEE01", "harness_chip_id"),
}

# Each chip's boot ROM, by its file in the ROM directory. The RP2040's is the
# B2 ROM's ELF, from which the bench script's extractor takes the mask ROM.
ROMS = {"rp2040": "b2.elf", "rp2350": "bootrom-combined.bin"}
# Instance N of the bench listens on these ports plus N, so several benches
# can run on one host, each with its own harness serial.
MONITOR_PORT = 1234
USBIP_PORT = 3240
USBIP_BUSID = "1-0"
INSTANCE_ENV = "METALCLAW_RENODE_INSTANCE"
MAX_INSTANCES = 8
# Held while an import is attached, so each bench can tell which vhci port is
# its own.
ATTACH_LOCK = Path(tempfile.gettempdir()) / "metalclaw-usbip-attach.lock"
STARTUP_TIMEOUT_S = 120.0
# Linux gives up enumerating after about 90 s of descriptor reads that time
# out; past this, an import with no device behind it is replugged.
REPLUG_AFTER_S = 120.0


class BenchError(RuntimeError):
    pass


def instance() -> int:
    value = os.environ.get(INSTANCE_ENV, "").strip() or "0"
    try:
        number = int(value)
    except ValueError:
        number = -1
    if not 0 <= number < MAX_INSTANCES:
        raise BenchError(f"invalid {INSTANCE_ENV} {value!r}; expected 0 to {MAX_INSTANCES - 1}")
    return number


# Instance 0's harness reports the layout's serial; another instance's needs a
# bench script that can set the chip ID.
def harness_serial(layout: Layout, number: int) -> str:
    if number == 0:
        return layout.serial
    if layout.chip_id_variable is None:
        raise BenchError(f"{layout.script} gives every harness the serial {layout.serial}, "
                         f"so only {INSTANCE_ENV}=0 can run on it")
    return f"{int(layout.serial, 16) + number:016X}"


@contextlib.contextmanager
def _attach_lock():
    with open(ATTACH_LOCK, "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def _listening(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1.0):
            return True
    except OSError:
        return False


def _wait_port(port: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while True:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError:
            if time.monotonic() >= deadline:
                raise BenchError(f"nothing listens on port {port} after {timeout:.0f} s")
            time.sleep(0.5)


VHCI_STATUS = Path("/sys/devices/platform/vhci_hcd.0/status")
VHCI_PORT_USED = 6


# The vhci ports holding an import, from the kernel's own table: `usbip port`
# stopped listing an import once the kernel gave up enumerating it.
def _imported_ports() -> list[int]:
    ports = []
    for line in VHCI_STATUS.read_text().splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 3 and int(fields[2]) == VHCI_PORT_USED:
            ports.append(int(fields[1]))
    return ports


def _enumerated(serial: str) -> bool:
    for device in Path("/sys/bus/usb/devices").iterdir():
        try:
            if (device / "idVendor").read_text().strip() == "2fe3" and (device / "serial").read_text().strip() == serial:
                return True
        except OSError:
            continue
    return False


class Cable(threading.Thread):
    # Re-enumeration (attaching a USB-facing device) unplugs the harness; on
    # the bench the cable stays in, so the import is renewed whenever the
    # exported bus is back and not imported. The kernel's descriptor timeouts
    # run in host time and the emulated harness is much slower, so while it
    # is busy the kernel can give up on it for good; then the cable is pulled
    # and plugged again, as on the bench.
    def __init__(self, busid: str, port: int, serial: str, log):
        super().__init__(daemon=True)
        self.target = f"usbip://127.0.0.1:{port}/{busid}"
        self.base = ["sudo", "-n", "usbip", "--tcp-port", str(port)]
        self.busid = busid
        self.serial = serial
        self.log = log
        self.stopping = threading.Event()
        self.imported = threading.Event()
        self.unenumerated_since = None
        # The vhci port this cable's import took; other benches' are not ours.
        self.vhci_port = None

    def _usbip(self, *args):
        try:
            r = subprocess.run(self.base + list(args), capture_output=True, text=True, timeout=5)
            return r.returncode, r.stdout + r.stderr
        except subprocess.TimeoutExpired:
            return 124, "usbip timed out"

    def _own_ports(self) -> list[int]:
        return [self.vhci_port] if self.vhci_port in _imported_ports() else []

    def run(self):
        while not self.stopping.is_set():
            ports = self._own_ports()
            if not ports:
                self.unenumerated_since = None
                with _attach_lock():
                    before = set(_imported_ports())
                    status, detail = self._usbip("attach", "-r", "127.0.0.1", "-b", self.busid)
                    taken = sorted(set(_imported_ports()) - before)
                if status == 0:
                    self.vhci_port = taken[0] if taken else None
                    self._note(f"imported {self.target}")
                    self.imported.set()
            else:
                self.imported.set()
                now = time.monotonic()
                if _enumerated(self.serial):
                    self.unenumerated_since = None
                elif self.unenumerated_since is None:
                    self.unenumerated_since = now
                elif now - self.unenumerated_since > REPLUG_AFTER_S:
                    self._note(f"not enumerated after {REPLUG_AFTER_S:.0f} s; replugging")
                    self.unenumerated_since = None
                    self._detach(ports)
            self.stopping.wait(0.25)

    def _note(self, message):
        self.log.write(f"usbip: {message}\n")
        self.log.flush()

    def _detach(self, ports):
        for port in ports:
            self._usbip("detach", "-p", str(port))

    def stop(self):
        self.stopping.set()
        self.join(timeout=5)
        self._detach(self._own_ports())


# The monitor commands that bring the bench up. The DUT image goes straight
# into the DUT's flash, as a probe download would leave it: downloading it
# through the emulated probe takes 20-30 minutes of host time.
def monitor_commands(script: Path, harness_rom: Path, harness_flash: Path, dut_rom: Path, dut_flash_bin: Path,
                     dut_machine: str, quantum: str | None = None, usbip_port: int | None = None,
                     chip_id: tuple[str, str] | None = None) -> list[str]:
    commands = [
        f"$harness_rom=@{harness_rom}",
        f"$harness_flash=@{harness_flash}",
        f"$dut_rom=@{dut_rom}",
        f"$usbip_port={USBIP_PORT if usbip_port is None else usbip_port}",
    ]
    if quantum:
        commands.append(f'$quantum="{quantum}"')
    if chip_id is not None:
        variable, serial = chip_id
        commands.append(f"${variable}=0x{serial}")
    # Through the DUT's XIP window, as the bench script loads the harness's:
    # the W25Q takes the image at flash offset 0, which the XIP window's reset
    # address translation puts at 0x10000000.
    return commands + [
        f"include @{script}",
        f'mach set "{dut_machine}"',
        f"xipMemory LoadBinary @{dut_flash_bin}",
        "start",
    ]


class Bench:
    def __init__(self, renode_dir: Path, layout: Layout, harness_uf2: Path, roms: Path, dut_image: Path,
                 work: Path, serial: str, instance: int = 0):
        self.renode_dir = renode_dir
        self.layout = layout
        self.serial = serial
        self.instance = instance
        self.monitor_port = None
        self.harness_uf2 = harness_uf2
        self.roms = roms
        self.dut_image = dut_image
        self.work = work
        self.process = None
        self.cable = None
        self.log = None

    # The extractor is the bench script's own and refuses to overwrite.
    def _rom(self, chip: str, name: str) -> Path:
        source = self.roms / ROMS[chip]
        if chip != "rp2040":
            return source
        out = self.work / name
        out.unlink(missing_ok=True)
        subprocess.run(["python3", str(self.renode_dir / MASK_ROM_EXTRACTOR), str(source), str(out)],
                       check=True, capture_output=True, text=True)
        return out

    def start(self) -> None:
        layout = self.layout
        for path in (self.renode_dir / "renode", self.renode_dir / layout.script, self.harness_uf2,
                     self.roms / ROMS[layout.harness_chip], self.roms / ROMS[layout.dut_chip], self.dut_image):
            if not path.is_file():
                raise BenchError(f"missing {path}")
        # A Renode left from an earlier run would answer on these ports, and
        # the run would drive its bench instead of this one.
        monitor_port, usbip_port = MONITOR_PORT + self.instance, USBIP_PORT + self.instance
        for port in (monitor_port, usbip_port):
            if _listening(port):
                raise BenchError(f"port {port} is already in use; another Renode is running "
                                 f"(another {INSTANCE_ENV} runs on its own ports)")
        try:
            contents = dut_flash.flash_contents(self.dut_image.read_bytes())
        except dut_flash.DutFlashError as error:
            raise BenchError(f"{self.dut_image}: {error}") from error
        try:
            harness_flash = storage.with_storage(uf2.flash_contents(self.harness_uf2.read_bytes(), layout.harness_chip))
        except (uf2.Uf2Error, storage.StorageError) as error:
            raise BenchError(f"{self.harness_uf2}: {error}") from error
        self.work.mkdir(parents=True, exist_ok=True)
        flash_bin = self.work / "dut_flash.bin"
        flash_bin.write_bytes(contents)
        payload = self.work / "harness_flash.bin"
        payload.write_bytes(harness_flash)
        harness_rom = self._rom(layout.harness_chip, "harness_rom.bin")
        dut_rom = self._rom(layout.dut_chip, "dut_rom.bin")
        self.log = (self.work / "renode.log").open("w")
        chip_id = None if layout.chip_id_variable is None else (layout.chip_id_variable, self.serial)
        commands = monitor_commands(self.renode_dir / layout.script, harness_rom, payload, dut_rom, flash_bin,
                                    layout.dut_machine, os.environ.get("METALCLAW_RENODE_QUANTUM"), usbip_port,
                                    chip_id)
        argv = [str(self.renode_dir / "renode"), "--disable-gui", "--plain", "--port", str(monitor_port)]
        for command in commands:
            argv += ["-e", command]
        self.process = subprocess.Popen(argv, cwd=self.renode_dir, stdout=self.log, stderr=subprocess.STDOUT,
                                        start_new_session=True)
        self.monitor_port = monitor_port
        _wait_port(usbip_port, STARTUP_TIMEOUT_S)
        self.cable = Cable(USBIP_BUSID, usbip_port, self.serial, self.log)
        self.cable.start()
        if not self.cable.imported.wait(STARTUP_TIMEOUT_S):
            raise BenchError(f"the harness's USB/IP bus {USBIP_BUSID} was never imported")

    def stop(self) -> None:
        if self.cable is not None:
            self.cable.stop()
        # Whether Renode outlived the session, in its own log: a harness that
        # left USB mid-session is either Renode gone or the import dropped.
        if self.process is not None and self.log is not None:
            code = self.process.poll()
            self.log.write("rig: Renode still running at the end of the session\n" if code is None else
                           f"rig: Renode exited with {code} before the end of the session\n")
            self.log.flush()
        if self.process is not None and self.process.poll() is None:
            try:
                with socket.create_connection(("127.0.0.1", self.monitor_port), timeout=5) as monitor:
                    monitor.sendall(b"quit\n")
                self.process.wait(timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait()
        if self.log is not None:
            self.log.close()
            self.log = None
