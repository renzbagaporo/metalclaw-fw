# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness alone in Renode, for its engines: its firmware on the emulated
# chip, its USB exported over USB/IP and imported into this host's kernel, so
# the host sees the same USB device a bench harness is, and a bridge on its
# GPIO bank to a stimulus in this process (test/engine), which drives its
# pins as a DUT would.

import atexit
import contextlib
import fcntl
import json
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path

from . import storage, uf2

MASK_ROM_EXTRACTOR = "scripts/multi-node/extract_rp2040_mask_rom.py"


# By the harness's MicroPython board: the harness-only script in the Renode
# package, the chip, and the USB serial the emulated harness reports, with
# the script variable that sets it where the script has one.
@dataclass(frozen=True)
class HarnessLayout:
    script: str
    chip: str
    serial: str
    chip_id_variable: str | None = None


LAYOUTS = {
    "rpi_pi_pico": HarnessLayout("scripts/multi-node/metalclaw_harness_native.resc", "rp2040", "0123456789ABCDEF"),
    "metalclaw_pico_rp2": HarnessLayout("scripts/multi-node/metalclaw_harness_rp2_native.resc", "rp2350",
                                        "00000000C0FFEE01", "harness_chip_id"),
}

# Each chip's boot ROM, by its file in the ROM directory. The RP2040's is the
# B2 ROM's ELF, from which the bench script's extractor takes the mask ROM.
ROMS = {"rp2040": "b2.elf", "rp2350": "bootrom-combined.bin"}
# Instance N of the bench listens on these ports plus N, so several benches
# can run on one host.
MONITOR_PORT = 1234
USBIP_PORT = 3240
USBIP_BUSID = "1-0"
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


class Bench:
    """The harness of `layout` in Renode, its GPIO bank bridged at `bridge`
    (a stimulus's socket); start() returns once its USB is imported."""

    def __init__(self, renode_dir: Path, layout: HarnessLayout, harness_uf2: Path, roms: Path, work: Path,
                 bridge: Path, instance: int = 0):
        self.renode_dir = renode_dir
        self.layout = layout
        self.serial = layout.serial
        self.instance = instance
        self.monitor_port = None
        self.harness_uf2 = harness_uf2
        self.roms = roms
        self.work = work
        self.bridge = bridge
        self.usbip_port = None
        self.process = None
        self.cable = None
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
                     self.roms / ROMS[layout.chip]):
            if not path.is_file():
                raise BenchError(f"missing {path}")
        # A Renode left from an earlier run would answer on these ports, and
        # the run would drive its harness instead of this one.
        monitor_port, usbip_port = MONITOR_PORT + self.instance, USBIP_PORT + self.instance
        for port in (monitor_port, usbip_port):
            if _listening(port):
                raise BenchError(f"port {port} is already in use; another Renode is running")
        try:
            flash = storage.with_storage(uf2.flash_contents(self.harness_uf2.read_bytes(), layout.chip))
        except (uf2.Uf2Error, storage.StorageError) as error:
            raise BenchError(f"{self.harness_uf2}: {error}") from error
        self.work.mkdir(parents=True, exist_ok=True)
        payload = self.work / "harness_flash.bin"
        payload.write_bytes(flash)
        rom = self._rom(layout.chip, "harness_rom.bin")
        commands = [f"$harness_rom=@{rom}", f"$harness_flash=@{payload}", f"$usbip_port={usbip_port}"]
        if layout.chip_id_variable is not None:
            commands.append(f"${layout.chip_id_variable}=0x{layout.serial}")
        commands += [f"include @{self.renode_dir / layout.script}", "start"]
        self.log = (self.work / "renode.log").open("w")
        self._hold_exits()
        env = dict(os.environ, METALCLAW_BRIDGE=str(self.bridge))
        self.process = subprocess.Popen(renode_argv(self.renode_dir / "renode", monitor_port, commands),
                                        cwd=self.renode_dir, stdout=self.log, stderr=subprocess.STDOUT,
                                        start_new_session=True, env=env)
        self._ended = False
        self.monitor_port = monitor_port
        self.usbip_port = usbip_port
        _wait_port(usbip_port, STARTUP_TIMEOUT_S)
        self.cable = Cable(USBIP_BUSID, usbip_port, self.serial, self.log)
        self.cable.start()
        if not self.cable.imported.wait(STARTUP_TIMEOUT_S):
            raise BenchError(f"the harness's USB/IP bus {USBIP_BUSID} was never imported")

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
        cable, self.cable = self.cable, None
        if cable is None:
            return
        try:
            cable.stop()
        except Exception as error:
            self._note(f"rig: could not release {cable.target}: {error}")
            raise

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
