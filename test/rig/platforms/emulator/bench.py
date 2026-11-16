# SPDX-License-Identifier: Apache-2.0 OR MIT
# The bench in Renode: the harness, the DUT and their wires in one emulation,
# with the harness's USB exported over USB/IP and imported into this host's
# kernel, so the host sees the same USB device the bench does.

import atexit
import contextlib
import fcntl
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass, replace
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
    # What the DUT's peripherals are called in `dut_machine`: "" when it is
    # a machine of its own, "d_" when both chips share one (each chip's
    # peripherals carry their prefix, its cores their own cluster).
    dut_prefix: str = ""


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

# The same benches with both chips in one Renode machine, "bench": the
# harness's and the DUT's peripherals on one clock, their pins joined by
# wires with no latency, each chip's cores and bus registrations in a
# cluster of its own. Chosen with METALCLAW_EMULATOR_LAYOUT=one_machine.
ONE_MACHINE_LAYOUTS = {
    "rpi_pi_pico": Layout("scripts/multi-node/metalclaw_fixture_one_machine.resc", "rp2040", "rp2350", "bench",
                          "0123456789ABCDEF", dut_prefix="d_"),
    "metalclaw_pico_rp2": Layout("scripts/multi-node/metalclaw_carrier_one_machine.resc", "rp2350", "rp2040",
                                 "bench", "00000000C0FFEE01", "harness_chip_id", dut_prefix="d_"),
}

# Each chip's boot ROM, by its file in the ROM directory. The RP2040's is the
# B2 ROM's ELF, from which the bench script's extractor takes the mask ROM.
ROMS = {"rp2040": "b2.elf", "rp2350": "bootrom-combined.bin"}
# Instance N of the bench listens on these ports plus N, so several benches
# can run on one host, each with its own harness serial.
MONITOR_PORT = 1234
USBIP_PORT = 3240
USBIP_BUSID = "1-0"
# The DUT's own USB device, on a USB/IP server of its own at DUT_USBIP_PORT
# plus the instance: Renode's server takes one connection at a time, and
# the harness's import holds its own server's for the whole session.
DUT_USBIP_PORT = 3250
DUT_USBIP_BUSID = "1-1"
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


# The device enumerated on a vhci port, as its sysfs name, if one is.
def _port_device(port: int) -> str | None:
    for line in VHCI_STATUS.read_text().splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 7 and int(fields[1]) == port and fields[6] != "0-0":
            return fields[6]
    return None


def _device_attribute(device: str, name: str) -> str | None:
    try:
        return (USB_DEVICES / device / name).read_text().strip()
    except OSError:
        return None


# The serial of the device enumerated on a vhci port, if one is.
def _port_serial(port: int) -> str | None:
    device = _port_device(port)
    return None if device is None else _device_attribute(device, "serial")


# The VID:PID of the device enumerated on a vhci port, if one is.
def _port_identity(port: int) -> tuple[int, int] | None:
    device = _port_device(port)
    if device is None:
        return None
    vid, pid = _device_attribute(device, "idVendor"), _device_attribute(device, "idProduct")
    try:
        return int(vid, 16), int(pid, 16)
    except (TypeError, ValueError):
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

    # Whether the device on a vhci port, while none has enumerated there yet
    # or once one has, can be the one this cable carries.
    def _carries(self, port: int) -> bool:
        return _port_serial(port) in (None, self.serial)

    def _enumerated(self, ports: list[int]) -> bool:
        return _enumerated(self.serial)

    # Called with the owners held. A port stops being ours once the kernel
    # frees it, or once it holds another device: an import made outside the
    # rig never takes the lock.
    def _own_ports(self, owners) -> list[int]:
        imported = _imported_ports()
        for port in [port for port, target in owners.items() if target == self.target]:
            if port not in imported or not self._carries(port):
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
            if self._enumerated(ports):
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


DEV = Path("/dev")


# What a USB device presents to this host once its drivers bound, from
# sysfs: its CDC ACM ttys as (interface string, device node) in interface
# order, its hidraw nodes, and its endpoints but EP0 as bEndpointAddress in
# two hex digits.
@dataclass(frozen=True)
class UsbFunctions:
    device: str
    vid: int
    pid: int
    ttys: tuple[tuple[str, str], ...] = ()
    hidraw: tuple[str, ...] = ()
    endpoints: tuple[str, ...] = ()


def _interface_number(interface: Path) -> int:
    try:
        return int((interface / "bInterfaceNumber").read_text().strip(), 16)
    except (OSError, ValueError):
        return -1


def usb_functions(device: str) -> UsbFunctions | None:
    try:
        vid = int(_device_attribute(device, "idVendor"), 16)
        pid = int(_device_attribute(device, "idProduct"), 16)
    except (TypeError, ValueError):
        return None
    ttys, hidraw, endpoints = [], [], set()
    for interface in sorted(USB_DEVICES.glob(f"{device}:*"), key=_interface_number):
        label = _device_attribute(interface.name, "interface") or ""
        ttys += [(label, str(DEV / tty.name)) for tty in sorted(interface.glob("tty/tty*"))]
        hidraw += [str(DEV / node.name) for node in sorted(interface.glob("*/hidraw/hidraw*"))]
        endpoints |= {ep.name[3:] for ep in interface.glob("ep_*")}
    return UsbFunctions(device, vid, pid, tuple(ttys), tuple(hidraw), tuple(sorted(endpoints)))


class DutCable(Cable):
    # The DUT's own USB, which the bench exports as a second device on the
    # harness's USB/IP server, as its USB connector would reach this host:
    # the program's device, known by its VID:PID, since its serial comes
    # from the DUT's flash.
    def __init__(self, busid: str, port: int, vid: int, pid: int, log):
        super().__init__(busid, port, f"{vid:04x}:{pid:04x}", log)
        self.identity = (vid, pid)

    def _carries(self, port: int) -> bool:
        return _port_identity(port) in (None, self.identity)

    def _enumerated(self, ports: list[int]) -> bool:
        return any(_port_identity(port) == self.identity for port in ports)

    # The program's device on this cable's import, once it enumerated.
    def functions(self) -> UsbFunctions | None:
        with _port_owners() as owners:
            ports = self._own_ports(owners)
        for port in ports:
            device = _port_device(port)
            if device is not None and _port_identity(port) == self.identity:
                return usb_functions(device)
        return None


# The variables a bench script takes, with their defaults: its `$name?=value`
# lines.
def script_variables(text: str) -> dict[str, str]:
    return dict(re.findall(r"^\$(\w+)\?=(.*?)\s*$", text, re.M))


@dataclass(frozen=True)
class Settings:
    # (name, value) set before the bench script is included.
    variables: tuple[tuple[str, str], ...] = ()
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
                   quantum: str | None = None, dut_prefix: str = "") -> Settings:
    # A program's quantum and wire latency tune the delayed wires between two
    # machines; with both chips in one machine (a DUT prefix) every GPIO is a
    # plain wire on one clock, at the script's own quantum, and they do not
    # apply. I2C0 still crosses as the script's byte-level bus unless the
    # program asks for pin wires (*_one_machine_i2c0_{bus,wires}.resc):
    # Renode's I2C controllers talk I2C only at byte level.
    if dut_prefix:
        emulation = replace(emulation, quantum=None, wire_latency_us=None)
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
    return Settings(tuple(variables), tuple(unmet))


# The monitor commands that bring the bench up. The DUT image goes straight
# into the DUT's flash, as a probe download would leave it: downloading it
# through the emulated probe takes 20-30 minutes of host time.
def monitor_commands(script: Path, harness_rom: Path, harness_flash: Path, dut_rom: Path, dut_flash_bin: Path,
                     dut_machine: str, settings: Settings | None = None, usbip_port: int | None = None,
                     chip_id: tuple[str, str] | None = None, dut_usbip_port: int | None = None,
                     dut_prefix: str = "") -> list[str]:
    settings = settings or Settings()
    commands = [
        f"$harness_rom=@{harness_rom}",
        f"$harness_flash=@{harness_flash}",
        f"$dut_rom=@{dut_rom}",
        f"$usbip_port={USBIP_PORT if usbip_port is None else usbip_port}",
    ]
    if dut_usbip_port is not None:
        commands.append(f"$dut_usbip_port={dut_usbip_port}")
    commands += [f"${name}={value}" for name, value in settings.variables]
    if chip_id is not None:
        variable, serial = chip_id
        commands.append(f"${variable}=0x{serial}")
    # Through the DUT's XIP window, as the bench script loads the harness's:
    # the W25Q takes the image at flash offset 0, which the XIP window's reset
    # address translation puts at 0x10000000.
    return commands + [
        f"include @{script}",
        f'mach set "{dut_machine}"',
        f"{dut_prefix}xipMemory LoadBinary @{dut_flash_bin}",
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
        self.usbip_port = None
        # The DUT's USB/IP server, where this package's bench script has one.
        self.dut_usbip_port = None
        self.process = None
        self.cable = None
        self.dut_cable = None
        self.log = None
        self._handlers = {}
        # Set once stop() has dealt with the Renode in `process`.
        self._ended = False

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

    # Whatever ends the start, an interrupt included, leaves no Renode
    # behind.
    def start(self) -> None:
        try:
            self._start()
        except BaseException:
            self.stop()
            raise

    def _start(self) -> None:
        layout = self.layout
        for path in (self.renode_dir / "renode", self.renode_dir / layout.script, self.harness_uf2,
                     self.roms / ROMS[layout.harness_chip], self.roms / ROMS[layout.dut_chip], self.dut_image):
            if not path.is_file():
                raise BenchError(f"missing {path}")
        script = self.renode_dir / layout.script
        script_text = script.read_text()
        # A Renode left from an earlier run would answer on these ports, and
        # the run would drive its bench instead of this one.
        monitor_port, usbip_port = MONITOR_PORT + self.instance, USBIP_PORT + self.instance
        dut_usbip_port = DUT_USBIP_PORT + self.instance if "dut_usbip_port" in script_variables(script_text) else None
        for port in (monitor_port, usbip_port, dut_usbip_port):
            if port is None:
                continue
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
        self.settings = bench_settings(self.emulation, script, script_text, layout.dut_machine,
                                       os.environ.get("METALCLAW_RENODE_QUANTUM"), layout.dut_prefix)
        self.log = (self.work / "renode.log").open("w")
        self._note_settings()
        chip_id = None if layout.chip_id_variable is None else (layout.chip_id_variable, self.serial)
        commands = monitor_commands(script, harness_rom, payload, dut_rom, flash_bin, layout.dut_machine,
                                    self.settings, usbip_port, chip_id, dut_usbip_port, layout.dut_prefix)
        argv = renode_argv(self.renode_dir / "renode", monitor_port, commands)
        self._hold_exits()
        self.process = subprocess.Popen(argv, cwd=self.renode_dir, stdout=self.log, stderr=subprocess.STDOUT,
                                        start_new_session=True)
        self._ended = False
        self.monitor_port = monitor_port
        self.usbip_port = usbip_port
        self.dut_usbip_port = dut_usbip_port
        _wait_port(usbip_port, STARTUP_TIMEOUT_S)
        self.cable = Cable(USBIP_BUSID, usbip_port, self.serial, self.log)
        self.cable.start()
        if not self.cable.imported.wait(STARTUP_TIMEOUT_S):
            raise BenchError(f"the harness's USB/IP bus {USBIP_BUSID} was never imported")

    def _note_settings(self) -> None:
        s = self.settings
        if s.variables:
            self.log.write("rig: bench variables " + " ".join(f"${n}={v}" for n, v in s.variables) + "\n")
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

    # What the bench lacks for the DUT's own USB, or None: a bench script
    # without $dut_usbip_port exports no USB/IP server for it.
    def dut_usb_unmet(self) -> str | None:
        if self.process is not None and self.dut_usbip_port is None:
            return f"$dut_usbip_port, which {self.layout.script} in this Renode package does not take"
        return None

    # The DUT's own USB, imported into this host: only while the emulation
    # runs, and once the program has its device up.
    def import_dut_usb(self, vid: int, pid: int) -> DutCable:
        if self.process is None or self.usbip_port is None:
            raise BenchError("the bench is not running")
        if self.dut_usbip_port is None:
            raise BenchError(f"the DUT's USB needs {self.dut_usb_unmet()}")
        if self.dut_cable is not None:
            raise BenchError("the DUT's USB is already imported")
        self.dut_cable = DutCable(DUT_USBIP_BUSID, self.dut_usbip_port, vid, pid, self.log)
        self.dut_cable.start()
        return self.dut_cable

    def release_dut_usb(self) -> None:
        cable, self.dut_cable = self.dut_cable, None
        if cable is not None:
            cable.stop()

    # Until stop(), every way out of the session stops the bench: an
    # interpreter exit through atexit, SIGTERM and SIGHUP as a KeyboardInterrupt
    # (which pytest answers with its teardown, as it does SIGINT), and a
    # SIGKILL of the rig through Renode's parent-death signal (renode_argv).
    def _hold_exits(self) -> None:
        atexit.register(self.stop)
        if threading.current_thread() is not threading.main_thread():
            return
        for number in EXIT_SIGNALS:
            if signal.getsignal(number) is signal.SIG_DFL:
                self._handlers[number] = signal.signal(number, _interrupt)

    def _release_exits(self) -> None:
        atexit.unregister(self.stop)
        handlers, self._handlers = self._handlers, {}
        for number, handler in handlers.items():
            signal.signal(number, handler)

    def stop(self) -> None:
        try:
            with _signals_deferred():
                try:
                    self._stop_cables()
                finally:
                    try:
                        self._stop_renode()
                    finally:
                        if self.log is not None:
                            self.log.close()
                            self.log = None
        finally:
            self._release_exits()

    def _stop_cables(self) -> None:
        cables = [c for c in (self.dut_cable, self.cable) if c is not None]
        self.dut_cable = self.cable = None
        failed = None
        for cable in cables:
            try:
                cable.stop()
            except Exception as error:
                self._note(f"rig: could not release {cable.target}: {error}")
                failed = failed or error
        if failed is not None:
            raise failed

    def _note(self, line: str) -> None:
        if self.log is not None:
            self.log.write(line + "\n")
            self.log.flush()

    # Whether Renode outlived the session, in its own log: a harness that
    # left USB mid-session is either Renode gone or the import dropped. A
    # Renode still running is asked to quit, then sent SIGTERM, then, as it
    # can ignore that, SIGKILL, each after its grace period.
    def _stop_renode(self) -> None:
        process = self.process
        if process is None or self._ended:
            return
        self._ended = True
        code = process.poll()
        self._note("rig: Renode still running at the end of the session" if code is None else
                   f"rig: Renode exited with {code} before the end of the session")
        if code is not None:
            return
        try:
            if self.monitor_port is None:
                raise OSError("no monitor port")
            with socket.create_connection(("127.0.0.1", self.monitor_port), timeout=5) as monitor:
                monitor.sendall(b"quit\n")
        except OSError:
            self._note("rig: Renode's monitor did not take quit; sending SIGTERM")
        else:
            if _waited(process, QUIT_GRACE_S):
                return
            self._note(f"rig: Renode did not quit within {QUIT_GRACE_S:.0f} s; sending SIGTERM")
        _signal_group(process, signal.SIGTERM)
        if _waited(process, TERM_GRACE_S):
            return
        self._note(f"rig: Renode outlived SIGTERM by {TERM_GRACE_S:.0f} s; sending SIGKILL")
        _signal_group(process, signal.SIGKILL)
        process.wait()


# Host seconds Renode gets to end after `quit`, then after SIGTERM.
QUIT_GRACE_S = 30.0
TERM_GRACE_S = 10.0
EXIT_SIGNALS = (signal.SIGTERM, signal.SIGHUP)
# util-linux's setpriv sets the parent-death signal and execs Renode, so a
# rig killed outright takes its Renode with it.
SETPRIV = shutil.which("setpriv")


def renode_argv(renode: Path, monitor_port: int, commands: list[str]) -> list[str]:
    argv = [str(renode), "--disable-gui", "--plain", "--port", str(monitor_port)]
    for command in commands:
        argv += ["-e", command]
    return ([SETPRIV, "--pdeathsig", "KILL"] if SETPRIV else []) + argv


def _interrupt(number, frame):
    raise KeyboardInterrupt(f"signal {signal.Signals(number).name}")


# Signals that arrive while the bench stops are held until it has: an
# interrupt part way through would leave Renode running. The first is then
# raised as the KeyboardInterrupt the session would have had, so it still
# ends as it was asked to. Handlers, not the signal mask: the usbip
# detaches the cables run would inherit a mask.
@contextlib.contextmanager
def _signals_deferred():
    if threading.current_thread() is not threading.main_thread():
        yield
        return
    held = []
    # A handler installed outside Python reads as None, and Python cannot
    # put it back: such a signal is left as it is.
    numbers = [n for n in (signal.SIGINT, *EXIT_SIGNALS) if signal.getsignal(n) is not None]
    previous = {n: signal.signal(n, lambda number, frame: held.append(number)) for n in numbers}
    try:
        yield
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)
        if held:
            raise KeyboardInterrupt(f"signal {signal.Signals(held[0]).name} while the bench stopped")


def _waited(process, seconds: float) -> bool:
    try:
        process.wait(timeout=seconds)
        return True
    except subprocess.TimeoutExpired:
        return False


# Renode runs in its own session, so its process group is its own.
def _signal_group(process, number: int) -> None:
    try:
        os.killpg(process.pid, number)
    except (ProcessLookupError, PermissionError):
        try:
            process.send_signal(number)
        except ProcessLookupError:
            pass
