# SPDX-License-Identifier: Apache-2.0 OR MIT
# The native platform: the DUT emulated in Renode, its parts served by the
# host harness (core/ports/host) through the bridge (BRIDGE.md). The host
# harness is Metalclaw's own runtime and models in a Linux process, so the
# cases attach parts and judge them as on a bench; the emulator holds only
# the DUT, so nothing waits on a second emulated chip.
#
# Environment:
#   METALCLAW_DUT_IMAGE       the DUT's ELF (with example.json beside it)
#   METALCLAW_RENODE_DIR      a Renode package with the bridge
#   METALCLAW_RENODE_ROMS     the boot ROMs, as for the emulator platform
#   METALCLAW_ARM_TOOLCHAIN_DIR  for objcopy
#   METALCLAW_HOST_HARNESS    the host harness (default build/host/...)
#   METALCLAW_NATIVE_WORK     a directory for the socket and logs
#   METALCLAW_RENODE_INSTANCE ports per instance, as for the emulator

import ast
import contextlib
import os
import re
import select
import socket
import subprocess
import threading
import time
from pathlib import Path

import pytest

from test.pico_examples.emulation import DEFAULT, Emulation, emulation_for
from test.rig.platforms.hardware.mailbox import RESULT_WORDS, Mailbox
from test.rig.platforms.hardware.platform import (
    FIXTURE,
    ROOT,
    Dut,
    _required,
    dut_image,
    example_metadata,
    fixture_for,
    require_example_image,
    soc_series,
)
from test.rig.platforms.emulator.bench import MASK_ROM_EXTRACTOR
from test.rig.platforms.hardware.repl import Repl
from test.rig.platforms.native.host import HostProcess, _raw_pty, host_harness_program

MONITOR_PORT = 1334
CONSOLE_PORT = 1434
# The RP2040 Pico's DUT-only script; the bridge connects from it.
SCRIPTS = {"rp2040": "scripts/multi-node/metalclaw_pico_native.resc"}
ROMS = {"rp2040": "b2.elf"}
START_TIMEOUT_S = 60.0
# Wall seconds the DUT's clock may stand still before a harness sleep gives
# up, as the console wait does (test/pico_examples/cases.py).
CLOCK_STALL_S = 120.0
REPL_STRETCH = 4.0
REPL_MIN_S = 20.0

# The DUT's pins are the harness's: the bridge names every line by the DUT
# pin it is on.
NATIVE_FIXTURE = {
    **FIXTURE,
    "dut_pins": {p: (p,) for p in range(30)},
    "analog_pins": (),
    "idle": {},
}


def instance() -> int:
    return int(os.environ.get("METALCLAW_RENODE_INSTANCE", "0"))


class Monitor:
    """Renode's monitor on its telnet port. The monitor echoes each command
    and ends its output with a prompt; a reply is what follows this
    command's echo up to the next prompt, so output that arrives late (the
    start-up commands') never answers a later command."""

    PROMPT = re.compile(rb"\(([\w-]+|monitor)\) $")

    def __init__(self, port: int, timeout: float = START_TIMEOUT_S):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5.0)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        self.buf = b""
        self._fill_until(lambda: self.PROMPT.search(self.buf[-64:]) is not None, timeout)

    def _fill_until(self, done, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while not done():
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError(f"Renode monitor: no reply after {self.buf[-300:]!r}")
            ready, _, _ = select.select([self.sock], [], [], left)
            if ready:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise EOFError("Renode monitor closed")
                self.buf += chunk

    def command(self, line: str, timeout: float = 30.0) -> str:
        self.buf = b""
        self.sock.sendall(line.encode() + b"\n")
        echo = line.encode()

        def answered() -> bool:
            at = self.buf.rfind(echo)
            return at >= 0 and self.PROMPT.search(self.buf[at + len(echo):][-64:]) is not None
        self._fill_until(answered, timeout)
        at = self.buf.rfind(echo)
        return self.buf[at + len(echo):].decode(errors="replace")

    def close(self) -> None:
        self.sock.close()


class ConsolePty:
    """The DUT's console from Renode's socket terminal, relayed to a raw
    pseudo-terminal the cases open by path, as a harness's serial port."""

    def __init__(self, port: int, log: Path, timeout: float = START_TIMEOUT_S):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5.0)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        # Everything the DUT printed, whether a case read it or not.
        self._log = open(log, "ab", buffering=0)
        self.master, self._slave = _raw_pty()
        self.path = os.ttyname(self._slave)
        self._stop_r, self._stop_w = os.pipe()
        self._thread = threading.Thread(target=self._forward, daemon=True)
        self._thread.start()

    def _forward(self) -> None:
        sock = self.sock.fileno()
        while True:
            ready, _, _ = select.select([sock, self.master, self._stop_r], [], [])
            if self._stop_r in ready:
                return
            try:
                if sock in ready:
                    data = self.sock.recv(65536)
                    if not data:
                        return
                    self._log.write(data)
                    os.write(self.master, data)
                if self.master in ready:
                    self.sock.sendall(os.read(self.master, 65536))
            except OSError:
                return

    def close(self) -> None:
        os.write(self._stop_w, b"x")
        self._thread.join(timeout=5)
        self.sock.close()
        self._log.close()
        for fd in (self.master, self._slave, self._stop_r, self._stop_w):
            os.close(fd)


def flat_image(image: Path, work: Path) -> Path:
    """The DUT image as the bytes of its flash from 0x10000000: the ELF's,
    or the Intel HEX a build names to download in its place (one that also
    erases a storage partition)."""
    metadata = example_metadata(image) or {}
    source, kind = image, "elf32-littlearm"
    if metadata.get("flash_image"):
        source, kind = image.parent / metadata["flash_image"], "ihex"
    objcopy = Path(_required("METALCLAW_ARM_TOOLCHAIN_DIR")).glob("*/bin/arm-none-eabi-objcopy")
    objcopy = next(iter(sorted(objcopy)), None)
    if objcopy is None:
        pytest.exit("no arm-none-eabi-objcopy under METALCLAW_ARM_TOOLCHAIN_DIR", returncode=2)
    out = work / "dut_flash.bin"
    subprocess.run([str(objcopy), "-I", kind, "-O", "binary", str(source), str(out)], check=True)
    return out


class NativeBench:
    def __init__(self, renode_dir: Path, roms: Path, image: Path, work: Path, number: int):
        self.renode_dir = renode_dir
        self.roms = roms
        self.image = image
        self.work = work
        self.instance = number
        self.chip = soc_series(image)
        self.monitor_port = MONITOR_PORT + number
        self.console_port = CONSOLE_PORT + number
        self.socket_path = work / "native.sock"
        # A Unix socket's path is at most 107 bytes; a deep work directory
        # gets one under the runtime directory instead.
        if len(str(self.socket_path)) > 100:
            runtime = Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp"))
            self.socket_path = runtime / f"metalclaw-native-{os.getpid()}-{number}.sock"
        self.host = None
        self.renode = None
        self.monitor = None
        self.console = None

    def start(self) -> None:
        if self.chip not in SCRIPTS:
            pytest.skip(f"native mode has no DUT script for {self.chip!r} yet")
        self.work.mkdir(parents=True, exist_ok=True)
        flash = flat_image(self.image, self.work)
        # The RP2040's boot ROM comes as an ELF; the script loads the 16 KiB
        # binary in it.
        rom = self.work / "dut_rom.bin"
        rom.unlink(missing_ok=True)
        subprocess.run(["python3", str(self.renode_dir / MASK_ROM_EXTRACTOR), str(self.roms / ROMS[self.chip]),
                        str(rom)], check=True, capture_output=True, text=True)
        self.host = HostProcess(host_harness_program(), self.socket_path, log=self.work / "host.log")
        commands = [
            f"$dut_rom=@{rom}",
            f"$console_port={self.console_port}",
            f"include @{self.renode_dir / SCRIPTS[self.chip]}",
            f"xipMemory LoadBinary @{flash}",
            "start",
        ]
        argv = [str(self.renode_dir / "renode"), "--disable-gui", "--plain", "--port", str(self.monitor_port)]
        for c in commands:
            argv += ["-e", c]
        env = {**os.environ, "METALCLAW_BRIDGE": str(self.socket_path)}
        with open(self.work / "renode.log", "ab") as log:
            self.renode = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT, env=env,
                                           start_new_session=True)
        self.monitor = Monitor(self.monitor_port)
        self.console = ConsolePty(self.console_port, self.work / "dut_console.log")

    def restart(self) -> None:
        self.monitor.command("machine Reset")
        self.monitor.command("start")

    # The DUT's cores stop; its time, and so the models', goes on, so a
    # part busy with what the DUT asked of it finishes. The next reset
    # releases them.
    def halt(self) -> None:
        for core in ("cpu0", "cpu1"):
            self.monitor.command(f"{core} IsHalted true")

    def stop(self) -> None:
        for close in (lambda: self.console and self.console.close(),
                      lambda: self.monitor and self.monitor.command("quit", timeout=5.0)):
            try:
                close()
            except Exception:
                pass
        if self.renode is not None:
            try:
                self.renode.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(self.renode.pid, 9)
                self.renode.wait()
        if self.host is not None:
            self.host.close()


class NativeHarness:
    """The host harness, as the cases use a harness: its REPL, and its
    clock, which is the DUT's virtual time."""

    def __init__(self, bench: NativeBench):
        self.bench = bench
        self._held = None

    # A REPL deadline is host time, shared with the emulator and the other
    # lanes on a loaded host: an attach that takes a second alone took over
    # five with seven lanes running.
    def run(self, code: str, timeout: float = 5.0) -> str:
        if self._held is None:
            self._held = Repl(path=self.bench.host.path).__enter__()
        return self._held.run(code, max(timeout * REPL_STRETCH, REPL_MIN_S))

    def board(self) -> str:
        return "native"

    def board_info(self) -> dict:
        return ast.literal_eval(self.run("import metalclaw\nprint(metalclaw.board())").strip())

    def clock(self) -> float:
        return int(self.run("import metalclaw\nprint(metalclaw.now_us())").strip()) / 1e6

    # Bench seconds: of the DUT's time, which may run slower than the host's.
    def sleep(self, seconds: float) -> None:
        start = last = self.clock()
        moved = time.monotonic()
        while last - start < seconds:
            time.sleep(0.01)
            now = self.clock()
            if now > last:
                last, moved = now, time.monotonic()
            elif time.monotonic() - moved > CLOCK_STALL_S:
                raise TimeoutError(f"the DUT's clock stood at {now:.3f} s for {CLOCK_STALL_S:.0f} s of wall time")

    def dut_console(self) -> str:
        return self.bench.console.path

    def close(self) -> None:
        if self._held is not None:
            self._held.__exit__(None, None, None)
            self._held = None


class MonitorMemory:
    """The DUT's memory through Renode's monitor, as the rig reads and
    writes it through a probe: read_words, write_words, and a link with
    the same two. Bytes go in bulk, little-endian."""

    BYTE = re.compile(r"0x([0-9A-Fa-f]{2})\b")
    CHUNK = 1024

    def __init__(self, bench: "NativeBench"):
        self.bench = bench

    def read_words(self, address: int, count: int, timeout=None) -> list[int]:
        data = b""
        while len(data) < 4 * count:
            n = min(self.CHUNK, 4 * count - len(data))
            out = self.bench.monitor.command(f"sysbus ReadBytes {address + len(data):#x} {n}")
            got = bytes(int(b, 16) for b in self.BYTE.findall(out))
            if len(got) != n:
                raise RuntimeError(f"Renode monitor: read {len(got)} of {n} bytes at {address + len(data):#x}")
            data += got
        return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]

    def write_words(self, address: int, words, timeout=None) -> None:
        data = b"".join((w & 0xFFFFFFFF).to_bytes(4, "little") for w in words)
        for at in range(0, len(data), self.CHUNK):
            part = data[at:at + self.CHUNK]
            self.bench.monitor.command(f"sysbus WriteBytes [{', '.join(f'{b:#04x}' for b in part)}] "
                                       f"{address + at:#x}")

    def write_when_reachable(self, address: int, words, seconds: float) -> None:
        self.write_words(address, words)

    def link(self):
        return contextlib.nullcontext(self)


class NativeExample:
    ASLEEP_MAX_S = 10.0

    def __init__(self, bench: NativeBench):
        self.bench = bench
        self.image = bench.image
        self.metadata = example_metadata(bench.image)
        self.target = self.metadata["target"]
        self.memory = MonitorMemory(bench)

    def parts_dut_pins(self) -> set[int]:
        from test.pico_examples.parts import parts_dut_pins
        return parts_dut_pins(self.target, self.metadata.get("board"))

    def restart(self) -> None:
        self.bench.restart()

    # The DUT stops where it is; the next restart runs it again.
    def halt_awake(self, wake_harness_pin, timeout) -> None:
        self.bench.halt()

    def reset_by_run(self, count: int, low_s: float, gap_s: float) -> None:
        pytest.skip("native mode has no RUN pin to press yet")


# Cases of the harness's own USB functions, which the host harness does not
# present: the native platform reaches the DUT through the emulator.
NATIVE_SKIPS = {
    "devices/swd_probe/": "the SWD probe is the harness's USB debug probe; native mode reaches the DUT's memory "
                          "through Renode's monitor instead",
    "devices/console_bridge/": "the console bridge is a USB serial function of the harness; native mode reads the "
                               "DUT's console from Renode",
}


def pytest_runtest_setup(item):
    for prefix, reason in NATIVE_SKIPS.items():
        if item.nodeid.startswith(prefix):
            pytest.skip(reason)


@pytest.fixture(scope="session")
def emulation() -> Emulation:
    try:
        return emulation_for(example_metadata(dut_image()))
    except pytest.exit.Exception:
        return DEFAULT


@pytest.fixture(scope="session")
def bench(pytestconfig):
    image = dut_image()
    work = Path(os.environ.get("METALCLAW_NATIVE_WORK", ROOT / "build/native")) / str(instance())
    b = NativeBench(Path(_required("METALCLAW_RENODE_DIR")), Path(_required("METALCLAW_RENODE_ROMS")),
                    image, work, instance())
    try:
        b.start()
        yield b
    finally:
        b.stop()


@pytest.fixture(scope="session")
def harness(bench) -> NativeHarness:
    h = NativeHarness(bench)
    yield h
    h.close()


@pytest.fixture(scope="session")
def fixture(harness: NativeHarness) -> dict:
    return fixture_for(NATIVE_FIXTURE, harness.board_info())


@pytest.fixture(scope="session")
def probe():
    return None


@pytest.fixture(scope="session")
def example(bench: NativeBench, harness: NativeHarness) -> NativeExample:
    require_example_image(bench.image)
    e = NativeExample(bench)
    e.restart()
    return e


@pytest.fixture(scope="module")
def dut_usb():
    pytest.skip("native mode does not bridge the DUT's own USB")


class NativeDut(Dut):
    """The rig's own DUT image, answering on its RAM mailbox, which the rig
    reaches through the monitor instead of a probe. The image is in the
    DUT's flash from the bench's start; a reset is the machine's."""

    def __init__(self, bench: NativeBench, harness: NativeHarness, toolchain_dir: str):
        self.bench = bench
        self.harness = harness
        self.image = bench.image
        self.memory = MonitorMemory(bench)
        self.mailbox = Mailbox(self.memory, bench.image, toolchain_dir, None)

    def flash(self, wake_harness_pin=None) -> None:
        self.reset()

    # A reset keeps the DUT's RAM, and a mailbox that still looks set up
    # would take a command before the rebooted DUT clears it; its result
    # words are cleared in between, so the rig waits for the DUT's own.
    # A reset resumes a running machine at once, so it is paused first.
    def reset(self) -> None:
        self.bench.monitor.command("pause")
        self.bench.monitor.command("machine Reset")
        self.memory.write_words(self.mailbox.result_address, [0] * RESULT_WORDS)
        self.bench.monitor.command("start")
        self.mailbox.last_result = 0

    def halt_awake(self, wake_harness_pin, timeout=None) -> None:
        self.bench.halt()

    def resume(self) -> None:
        pass

    def reset_by_run(self, count: int, low_s: float, gap_s: float) -> None:
        pytest.skip("native mode has no RUN pin to press yet")


@pytest.fixture(scope="session")
def dut(bench: NativeBench, harness: NativeHarness, fixture: dict) -> NativeDut:
    if example_metadata(bench.image) is not None:
        pytest.skip("the DUT image is a pico-examples program, not a rig image")
    d = NativeDut(bench, harness, _required("METALCLAW_ARM_TOOLCHAIN_DIR"))
    d.reset()
    d.ping()
    return d
