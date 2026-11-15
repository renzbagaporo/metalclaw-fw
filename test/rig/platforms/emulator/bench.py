# SPDX-License-Identifier: Apache-2.0 OR MIT
# The bench in Renode: the harness, the DUT and their wires in one emulation,
# with the harness's USB exported over USB/IP and imported into this host's
# kernel, so the host sees the same USB device the bench does.

import contextlib
import fcntl
import json
import os
import re
import socket
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from pathlib import Path

from test.pico_examples.emulation import DEFAULT, Emulation

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
# Held while a bench attaches, checks or detaches its import, and records which
# bench's import took each vhci port: the kernel's table shows every import
# alike, and the next attach takes the lowest free port, so another bench's
# import can land on our port number between two of our polls.
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


# The vhci ports' owners, by port, as each owner's USB/IP target; changes are
# saved when the lock is released.
@contextlib.contextmanager
def _port_owners():
    with open(ATTACH_LOCK, "a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        lock.seek(0)
        try:
            owners = {int(port): target for port, target in json.loads(lock.read() or "{}").items()}
        except (ValueError, AttributeError):
            owners = {}
        saved = dict(owners)
        yield owners
        if owners != saved:
            lock.truncate(0)
            lock.write(json.dumps(owners))


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
# An import holds its port from the attach on: NOTASSIGNED until the hub gives
# the device an address, then USED.
VHCI_PORT_HELD = (5, 6)


# The vhci ports holding an import, from the kernel's own table: `usbip port`
# stopped listing an import once the kernel gave up enumerating it.
def _imported_ports() -> list[int]:
    ports = []
    for line in VHCI_STATUS.read_text().splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 3 and int(fields[2]) in VHCI_PORT_HELD:
            ports.append(int(fields[1]))
    return ports


USB_DEVICES = Path("/sys/bus/usb/devices")
SERIAL_BY_ID = Path("/dev/serial/by-id")


# The serial of the device enumerated on a vhci port, if one is.
def _port_serial(port: int) -> str | None:
    for line in VHCI_STATUS.read_text().splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 7 and int(fields[1]) == port and fields[6] != "0-0":
            try:
                return (USB_DEVICES / fields[6] / "serial").read_text().strip()
            except OSError:
                return None
    return None


# Enumerated means the kernel lists the harness and its REPL tty is up. A
# harness the kernel is resetting stays listed while every descriptor read
# times out, and only its tty is gone.
def _enumerated(serial: str) -> bool:
    listed = False
    for device in USB_DEVICES.iterdir():
        try:
            if (device / "idVendor").read_text().strip() == "2fe3" and (device / "serial").read_text().strip() == serial:
                listed = True
                break
        except OSError:
            continue
    return listed and any(SERIAL_BY_ID.glob(f"usb-Metalclaw_*_{serial}-if00"))


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

    # usbip runs as root under sudo, so only sudo can signal it: sudo relays
    # SIGTERM to its command but cannot relay SIGKILL. A usbip that outlives
    # its timeout gets SIGTERM through sudo, and sudo is reaped either way.
    USBIP_TIMEOUT_S = 5
    USBIP_TERM_GRACE_S = 2

    def _usbip(self, *args):
        process = subprocess.Popen(self.base + list(args), stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        try:
            out, _ = process.communicate(timeout=self.USBIP_TIMEOUT_S)
            return process.returncode, out
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.communicate(timeout=self.USBIP_TERM_GRACE_S)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
            return 124, "usbip timed out"

    # Called with the owners held. A port stops being ours once the kernel
    # frees it, or once it holds a device with another serial: an import made
    # outside the rig never takes the lock.
    def _own_ports(self, owners) -> list[int]:
        imported = _imported_ports()
        for port in [port for port, target in owners.items() if target == self.target]:
            if port not in imported or _port_serial(port) not in (None, self.serial):
                del owners[port]
        return [port for port, target in owners.items() if target == self.target]

    def run(self):
        while not self.stopping.is_set():
            with _port_owners() as owners:
                self._poll(owners)
            self.stopping.wait(0.25)

    def _poll(self, owners):
        ports = self._own_ports(owners)
        if not ports:
            self.unenumerated_since = None
            before = set(_imported_ports())
            status, detail = self._usbip("attach", "-r", "127.0.0.1", "-b", self.busid)
            taken = sorted(set(_imported_ports()) - before)
            if status == 0:
                if taken:
                    owners[taken[0]] = self.target
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
                self._detach(ports, owners)

    def _note(self, message):
        self.log.write(f"usbip: {message}\n")
        self.log.flush()

    def _detach(self, ports, owners):
        for port in ports:
            self._usbip("detach", "-p", str(port))
            owners.pop(port, None)

    def stop(self):
        self.stopping.set()
        self.join(timeout=5)
        with _port_owners() as owners:
            self._detach(self._own_ports(owners), owners)


# The variables a bench script takes, with their defaults: its `$name?=value`
# lines.
def script_variables(text: str) -> dict[str, str]:
    return dict(re.findall(r"^\$(\w+)\?=(.*?)\s*$", text, re.M))


@dataclass(frozen=True)
class Settings:
    # (name, value) set before the bench script is included.
    variables: tuple[tuple[str, str], ...] = ()
    # Monitor commands after it, before the DUT's flash is loaded.
    commands: tuple[str, ...] = ()
    # What the program needs that this bench script does not take.
    unmet: tuple[str, ...] = ()


def _seconds(value: str, what: str) -> Decimal:
    try:
        seconds = Decimal(value.strip().strip('"'))
    except InvalidOperation:
        raise BenchError(f"invalid {what} {value!r}; expected seconds") from None
    if not seconds.is_finite() or seconds <= 0:
        raise BenchError(f"invalid {what} {value!r}; expected positive seconds")
    return seconds


# What a program's Emulation (test/pico_examples/emulation.py) sets on the
# bench `script`. A variable the script does not declare, or an I2C0 wiring
# whose script is not in the package, is left out and named in `unmet`. A
# wire latency also sets the DUT's SPI input delay, the round trip, where the
# script has one. `quantum` (METALCLAW_RENODE_QUANTUM) overrides the
# program's.
def bench_settings(emulation: Emulation, script: Path, script_text: str, dut_machine: str,
                   quantum: str | None = None) -> Settings:
    declared = script_variables(script_text)
    variables, unmet = [], []

    def need(name: str, value: str, shown) -> None:
        if name in declared:
            variables.append((name, value))
        else:
            unmet.append(f"${name} {shown}")

    if quantum:
        variables.append(("quantum", f'"{quantum}"'))
    elif emulation.quantum is not None:
        need("quantum", f'"{emulation.quantum}"', emulation.quantum)
    latency = emulation.wire_latency_us
    if latency is not None:
        if isinstance(latency, bool) or not isinstance(latency, int) or latency <= 0:
            raise BenchError(f"invalid wire latency {latency!r}; expected whole microseconds")
        need("wire_latency_us", str(latency), latency)
        if "spi_input_delay_ns" in declared:
            variables.append(("spi_input_delay_ns", str(2000 * latency)))
    set_quantum = dict(variables).get("quantum", declared.get("quantum"))
    set_latency = dict(variables).get("wire_latency_us", declared.get("wire_latency_us"))
    if (quantum or emulation.quantum or latency) and set_quantum is not None and set_latency is not None:
        q = _seconds(set_quantum, "quantum")
        if Decimal(set_latency) / 1_000_000 <= 2 * q:
            raise BenchError(f"a wire latency of {set_latency} us does not exceed two quanta of {q} s")
    wiring = emulation.i2c0_wiring
    if wiring is not None:
        if wiring not in ("bus", "wires"):
            raise BenchError(f"invalid I2C0 wiring {wiring!r}; expected bus or wires")
        wiring_script = script.with_name(f"{script.stem}_i2c0_{wiring}.resc")
        if "i2c0_wiring" in declared and wiring_script.is_file():
            variables.append(("i2c0_wiring", f"@{wiring_script}"))
        elif wiring != "bus":
            # The scripts' own I2C0 is the byte-level bus.
            unmet.append(f"$i2c0_wiring {wiring} ({wiring_script.name})")
    commands = []
    for s in emulation.stand_ins:
        commands += [f'emulation CreateDigitalWire "{s.wire}" false', f'mach set "{dut_machine}"',
                     f"{s.wire} Connect gpio {s.a}", f"{s.wire} Connect gpio {s.b}"]
    return Settings(tuple(variables), tuple(commands), tuple(unmet))


# The monitor commands that bring the bench up. The DUT image goes straight
# into the DUT's flash, as a probe download would leave it: downloading it
# through the emulated probe takes 20-30 minutes of host time.
def monitor_commands(script: Path, harness_rom: Path, harness_flash: Path, dut_rom: Path, dut_flash_bin: Path,
                     dut_machine: str, settings: Settings | None = None, usbip_port: int | None = None,
                     chip_id: tuple[str, str] | None = None) -> list[str]:
    settings = settings or Settings()
    commands = [
        f"$harness_rom=@{harness_rom}",
        f"$harness_flash=@{harness_flash}",
        f"$dut_rom=@{dut_rom}",
        f"$usbip_port={USBIP_PORT if usbip_port is None else usbip_port}",
    ]
    commands += [f"${name}={value}" for name, value in settings.variables]
    if chip_id is not None:
        variable, serial = chip_id
        commands.append(f"${variable}=0x{serial}")
    # Through the DUT's XIP window, as the bench script loads the harness's:
    # the W25Q takes the image at flash offset 0, which the XIP window's reset
    # address translation puts at 0x10000000.
    return commands + [
        f"include @{script}",
        *settings.commands,
        f'mach set "{dut_machine}"',
        f"xipMemory LoadBinary @{dut_flash_bin}",
        "start",
    ]


# The bench script's delayed UART0 wires, whose LateDeliveries count the
# edges each delivered later than its latency once the receiving board had
# run past them. Read before the bench quits; a Renode package whose script
# or wires lack them reports None for the wire.
LATE_DELIVERY_WIRES = ("uart0_dut_tx", "uart0_harness_tx")
MONITOR_TIMEOUT_S = 30.0
# The monitor's prompt, "(monitor) " or "(<machine>) ", ends every reply.
_PROMPT = re.compile(r"\([^()\r\n]*\) ?$")
_LEADING_PROMPT = re.compile(r"^\([^()\r\n]*\) ?")
# Telnet option negotiation, then terminal escapes.
_TELNET = re.compile(r"\xff[\xfb-\xfe].|\xff[^\xfb-\xfe]")
_ESCAPE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
_NUMBER = re.compile(r"(?:0x[0-9a-fA-F]+|[0-9]+)")


def _monitor_text(data: bytes) -> str:
    return _ESCAPE.sub("", _TELNET.sub("", data.decode("latin-1"))).replace("\r", "")


# The value a monitor command printed: its reply's one line that is a number
# (Renode 1.17 prints a counter as 0x0000000000000003), or None when it
# printed an error instead: "No such command or device" for an object the
# bench script never made, "does not provide a field, method or property"
# for a counter this package's wires do not have.
def monitor_value(reply: str, command: str) -> int | None:
    lines = [_LEADING_PROMPT.sub("", line).strip() for line in reply.split("\n")]
    lines = [line for line in lines if line and line != command]
    numbers = [line for line in lines if _NUMBER.fullmatch(line)]
    if len(numbers) != 1 or len(lines) != 1:
        return None
    return int(numbers[0], 0)


# Each command's reply, up to the prompt after it, from the monitor on
# `port`. A reply that never ends in a prompt is None, as is every later one.
# The greeting is read until it goes quiet: it can hold several prompts.
def monitor_replies(port: int, commands: list[str], timeout: float = MONITOR_TIMEOUT_S,
                    quiet: float = 0.5) -> list[str | None]:
    replies: list[str | None] = []
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as monitor:
        def until_prompt(settle: float = 0.0) -> str | None:
            text, deadline = "", time.monotonic() + timeout
            while True:
                prompted = _PROMPT.search(text) is not None
                if prompted and not settle:
                    return text
                left = deadline - time.monotonic()
                if left <= 0:
                    return text if prompted else None
                monitor.settimeout(min(left, settle) if prompted else left)
                try:
                    data = monitor.recv(4096)
                except socket.timeout:
                    if prompted:
                        return text
                    return None
                if not data:
                    return text if prompted else None
                text += _monitor_text(data)

        if until_prompt(quiet) is None:
            return [None] * len(commands)
        for command in commands:
            monitor.sendall(command.encode() + b"\n")
            reply = until_prompt()
            replies.append(reply)
            if reply is None:
                break
    return replies + [None] * (len(commands) - len(replies))


class Bench:
    def __init__(self, renode_dir: Path, layout: Layout, harness_uf2: Path, roms: Path, dut_image: Path,
                 work: Path, serial: str, instance: int = 0, emulation: Emulation = DEFAULT):
        self.renode_dir = renode_dir
        self.emulation = emulation
        self.settings = Settings()
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
            elf = self.dut_image.read_bytes()
            # A pico-examples program may run from SRAM, leaving flash erased.
            if (self.dut_image.parent / "example.json").is_file():
                contents = dut_flash.flash_contents(elf, allow_empty=True)
            else:
                contents = dut_flash.flash_contents(elf)
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
        script = self.renode_dir / layout.script
        self.settings = bench_settings(self.emulation, script, script.read_text(), layout.dut_machine,
                                       os.environ.get("METALCLAW_RENODE_QUANTUM"))
        self.log = (self.work / "renode.log").open("w")
        self._note_settings()
        chip_id = None if layout.chip_id_variable is None else (layout.chip_id_variable, self.serial)
        commands = monitor_commands(script, harness_rom, payload, dut_rom, flash_bin, layout.dut_machine,
                                    self.settings, usbip_port, chip_id)
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

    def _note_settings(self) -> None:
        s = self.settings
        if s.variables:
            self.log.write("rig: bench variables " + " ".join(f"${n}={v}" for n, v in s.variables) + "\n")
        if self.emulation.stand_ins:
            self.log.write("rig: emulator-only stand-ins " + " ".join(x.wire for x in self.emulation.stand_ins)
                           + "\n")
        if s.unmet:
            self.log.write(f"rig: {self.layout.script} does not take " + ", ".join(s.unmet) + "\n")
        self.log.flush()

    # Each delayed UART0 wire's LateDeliveries, None where this package's
    # bench has no such count or the monitor did not answer.
    def late_deliveries(self) -> dict[str, int | None]:
        counts: dict[str, int | None] = {wire: None for wire in LATE_DELIVERY_WIRES}
        if self.process is None or self.process.poll() is not None or self.monitor_port is None:
            return counts
        commands = [f"{wire} LateDeliveries" for wire in LATE_DELIVERY_WIRES]
        try:
            replies = monitor_replies(self.monitor_port, commands)
        except OSError:
            return counts
        for wire, command, reply in zip(LATE_DELIVERY_WIRES, commands, replies):
            counts[wire] = None if reply is None else monitor_value(reply, command)
        return counts

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
