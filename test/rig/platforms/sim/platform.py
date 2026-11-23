# SPDX-License-Identifier: Apache-2.0 OR MIT
# The sim platform: the DUT emulated in Renode, its parts served by the
# host harness (core/ports/host) through the bridge (BRIDGE.md). The host
# harness is Metalclaw's own runtime and models in a Linux process, so the
# cases attach parts and judge them as on a bench; the emulator holds only
# the DUT, so nothing waits on a second emulated chip.
#
# Environment:
#   METALCLAW_DUT_IMAGE       the DUT's ELF (with example.json beside it)
#   METALCLAW_RENODE_DIR      a Renode package with the bridge
#   METALCLAW_RENODE_ROMS     the boot ROMs
#   METALCLAW_ARM_TOOLCHAIN_DIR  for objcopy
#   METALCLAW_HOST_HARNESS    the host harness (default build/host/...)
#   METALCLAW_SIM_WORK        a directory for the socket and logs
#   METALCLAW_RENODE_INSTANCE ports per instance, so several can run at once

import ast
import contextlib
import os
import re
import select
import socket
import shutil
import subprocess
import threading
import time
from pathlib import Path

import pytest

from test.rig.platforms.hardware.mailbox import RESULT_WORDS, Mailbox
from test.rig.platforms.hardware.platform import (
    FIXTURE,
    ROOT,
    Dut,
    _required,
    dut_image,
    example_metadata,
    fixture_for,
    require_sample_image,
    soc_series,
)
from test.rig.renode.bench import MASK_ROM_EXTRACTOR
from test.rig.platforms.hardware.repl import Repl
from test.rig.platforms.sim.host import HostProcess, _raw_pty, host_harness_program

MONITOR_PORT = 1334
CONSOLE_PORT = 1434
# The RP2040 Pico's DUT-only script; the bridge connects from it.
SCRIPTS = {"rp2040": "scripts/multi-node/metalclaw_pico_native.resc",
           "rp2350": "scripts/multi-node/metalclaw_pico2_native.resc"}
ROMS = {"rp2040": "b2.elf", "rp2350": "bootrom-combined.bin"}
START_TIMEOUT_S = 60.0
# Wall seconds the DUT's clock may stand still before a harness sleep gives
# up.
CLOCK_STALL_S = 120.0
REPL_STRETCH = 4.0
REPL_MIN_S = 20.0

# The DUT's pins are the harness's: the bridge names every line by the DUT
# pin it is on.
SIM_FIXTURE = {
    **FIXTURE,
    # The harness's side of the DUT's console: its TX is the DUT's RX (GP1).
    "uart": {"tx": 1, "rx": 0},
    "dut_pins": {p: (p,) for p in range(30)},
    "analog_pins": (),
    # As the pico-rp2 fixture's: the sx126x's BUSY, which reads busy while
    # no radio drives it. The host harness pulls it low from the start
    # (SimBench), and a radio attached on it drives it.
    "idle": {10: 0},
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

    # Word reads in one round trip. Sent together, the commands' echoes
    # come before their answers, so each answer is taken as what it alone
    # is: a line holding one 32-bit hex value, in order.
    WORD = re.compile(rb"(?m)^\s*0x([0-9A-Fa-f]{8})\s*$")

    def read_words_at(self, addresses, timeout: float = 30.0) -> list[int]:
        self.buf = b""
        self.sock.sendall("".join(f"sysbus ReadDoubleWord {a:#x}\n" for a in addresses).encode())
        self._fill_until(lambda: len(self.WORD.findall(self.buf)) >= len(addresses), timeout)
        return [int(w, 16) for w in self.WORD.findall(self.buf)[:len(addresses)]]

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
    """The DUT image as the bytes of its flash from 0x10000000."""
    objcopy = Path(_required("METALCLAW_ARM_TOOLCHAIN_DIR")).glob("*/bin/arm-none-eabi-objcopy")
    objcopy = next(iter(sorted(objcopy)), None)
    if objcopy is None:
        pytest.exit("no arm-none-eabi-objcopy under METALCLAW_ARM_TOOLCHAIN_DIR", returncode=2)
    out = work / "dut_flash.bin"
    subprocess.run([str(objcopy), "-I", "elf32-littlearm", "-O", "binary", str(image), str(out)], check=True)
    return out


class SimBench:
    def __init__(self, renode_dir: Path, roms: Path, image: Path, work: Path, number: int):
        self.renode_dir = renode_dir
        self.roms = roms
        self.image = image
        self.work = work
        self.instance = number
        self.chip = soc_series(image)
        self.monitor_port = MONITOR_PORT + number
        self.console_port = CONSOLE_PORT + number
        self.socket_path = work / "sim.sock"
        # A Unix socket's path is at most 107 bytes; a deep work directory
        # gets one under the runtime directory instead.
        if len(str(self.socket_path)) > 100:
            runtime = Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp"))
            self.socket_path = runtime / f"metalclaw-sim-{os.getpid()}-{number}.sock"
        self.host = None
        self.renode = None
        self.monitor = None
        self.console = None

    def start(self) -> None:
        if self.chip not in SCRIPTS:
            pytest.skip(f"the sim platform has no DUT script for {self.chip!r} yet")
        self.work.mkdir(parents=True, exist_ok=True)
        flash = flat_image(self.image, self.work)
        # The RP2040's boot ROM comes as an ELF; the script loads the 16 KiB
        # binary in it. The RP2350's is a binary already.
        rom = self.work / "dut_rom.bin"
        rom.unlink(missing_ok=True)
        if ROMS[self.chip].endswith(".elf"):
            subprocess.run(["python3", str(self.renode_dir / MASK_ROM_EXTRACTOR), str(self.roms / ROMS[self.chip]),
                            str(rom)], check=True, capture_output=True, text=True)
        else:
            shutil.copyfile(self.roms / ROMS[self.chip], rom)
        self.host = HostProcess(host_harness_program(), self.socket_path, log=self.work / "host.log",
                                usb_dir=self.work / "usb",
                                idle_low=[p for p, level in SIM_FIXTURE["idle"].items() if level == 0])
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


class SimHarness:
    """The host harness, as the cases use a harness: its REPL, and its
    clock, which is the DUT's virtual time."""

    def __init__(self, bench: SimBench):
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
        return "sim"

    def board_info(self) -> dict:
        return ast.literal_eval(self.run("import metalclaw\nprint(metalclaw.board())").strip())

    # The DUT's virtual time, which a case's pause() of the simulation clock
    # leaves running, as a bench harness's own clock does.
    def clock(self) -> float:
        return int(self.run("import metalclaw\nprint(metalclaw.time_us())").strip()) / 1e6

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

    # A serial function the harness serves for an attached device, by its
    # interface string: the host's terminal for it.
    def serial_port(self, interface: str, timeout: float = 15.0) -> str:
        link = self.bench.host.usb_dir / interface
        deadline = time.monotonic() + timeout * REPL_STRETCH
        while not link.exists():
            if time.monotonic() >= deadline:
                raise TimeoutError(f"the host harness serves no serial port {interface!r}")
            time.sleep(0.05)
        return os.path.realpath(link)

    def wait_interface(self, name: str, present: bool = True, timeout: float = 15.0) -> None:
        link = self.bench.host.usb_dir / name
        deadline = time.monotonic() + timeout * REPL_STRETCH
        while link.exists() != present:
            if time.monotonic() >= deadline:
                raise TimeoutError(f"the host harness did not {'add' if present else 'drop'} {name!r}")
            time.sleep(0.05)

    def close(self) -> None:
        if self._held is not None:
            self._held.__exit__(None, None, None)
            self._held = None


class DutClock:
    """The DUT's seconds, for the mailbox's deadlines: the DUT runs slower
    than the host, so a deadline in bench seconds is one of its time. A
    clock that stands still for CLOCK_STALL_S of wall time raises."""

    def __init__(self, harness: "SimHarness"):
        self.harness = harness
        self.last = None
        self.moved = time.monotonic()

    def __call__(self) -> float:
        now = self.harness.clock()
        if now != self.last:
            self.last, self.moved = now, time.monotonic()
        elif time.monotonic() - self.moved > CLOCK_STALL_S:
            raise TimeoutError(f"the DUT's clock stood at {now:.3f} s for {CLOCK_STALL_S:.0f} s of wall time")
        return now


class MonitorMemory:
    """The DUT's memory through Renode's monitor, as the rig reads and
    writes it through a probe: read_words, write_words, and a link with
    the same two. Bytes go in bulk, little-endian."""

    BYTE = re.compile(r"0x([0-9A-Fa-f]{2})\b")
    CHUNK = 1024

    def __init__(self, bench: "SimBench"):
        self.bench = bench

    # Peripheral registers (from 0x40000000 on: the RP2040's and RP2350's
    # APB, AHB and SIO) take whole words, as a probe reads them.
    PERIPHERALS = 0x40000000

    def read_words(self, address: int, count: int, timeout=None) -> list[int]:
        if address >= self.PERIPHERALS:
            words = []
            for i in range(count):
                out = self.bench.monitor.command(f"sysbus ReadDoubleWord {address + 4 * i:#x}")
                found = re.findall(r"0x[0-9A-Fa-f]+", out)
                if not found:
                    raise RuntimeError(f"Renode monitor: no word read at {address + 4 * i:#x}: {out!r}")
                words.append(int(found[-1], 16))
            return words
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
        if address >= self.PERIPHERALS:
            for i, word in enumerate(words):
                self.bench.monitor.command(f"sysbus WriteDoubleWord {address + 4 * i:#x} {word & 0xFFFFFFFF:#x}")
            return
        data = b"".join((w & 0xFFFFFFFF).to_bytes(4, "little") for w in words)
        for at in range(0, len(data), self.CHUNK):
            part = data[at:at + self.CHUNK]
            self.bench.monitor.command(f"sysbus WriteBytes [{', '.join(f'{b:#04x}' for b in part)}] "
                                       f"{address + at:#x}")

    # One word at each address, in one round trip: a pin watch's sample.
    def read_scattered(self, addresses) -> list[int]:
        return self.bench.monitor.read_words_at(list(addresses))

    # The DUT held, and run a given time at a time: a pin watch's even
    # samples. Released, it runs freely again.
    def hold(self) -> None:
        self.bench.monitor.command("pause")

    def advance(self, seconds: float) -> None:
        self.bench.monitor.command(f'emulation RunFor "{seconds:.6f}"', timeout=60.0)

    def release(self) -> None:
        self.bench.monitor.command("start")

    def write_when_reachable(self, address: int, words, seconds: float) -> None:
        self.write_words(address, words)

    def link(self):
        return contextlib.nullcontext(self)


class SimSample:
    ASLEEP_MAX_S = 10.0

    def __init__(self, bench: SimBench):
        self.bench = bench
        self.image = bench.image
        self.metadata = example_metadata(bench.image)
        self.target = self.metadata["target"]
        self.memory = MonitorMemory(bench)

    def restart(self) -> None:
        self.bench.restart()

    # The DUT stops where it is; the next restart runs it again.
    def halt_awake(self, wake_harness_pin, timeout) -> None:
        self.bench.halt()


# Cases of the harness's own USB functions, which the host harness does not
# present: the sim platform reaches the DUT through Renode.
SIM_SKIPS = {
    "devices/swd_probe/": "the SWD probe is the harness's USB debug probe; the sim platform reaches the DUT's memory "
                          "through Renode's monitor instead",
    "devices/console_bridge/": "its cases are of the harness's USB serial function (interface numbers, line coding, "
                               "DTR); the sim platform serves it as a terminal, which reads the DUT's console and no more",
}


def pytest_runtest_setup(item):
    for prefix, reason in SIM_SKIPS.items():
        if item.nodeid.startswith(prefix):
            pytest.skip(reason)


@pytest.fixture(scope="session")
def bench(pytestconfig):
    image = dut_image()
    work = Path(os.environ.get("METALCLAW_SIM_WORK", ROOT / "build/sim")) / str(instance())
    b = SimBench(Path(_required("METALCLAW_RENODE_DIR")), Path(_required("METALCLAW_RENODE_ROMS")),
                    image, work, instance())
    try:
        b.start()
        yield b
    finally:
        b.stop()


@pytest.fixture(scope="session")
def harness(bench) -> SimHarness:
    h = SimHarness(bench)
    yield h
    h.close()


@pytest.fixture(scope="session")
def fixture(harness: SimHarness) -> dict:
    return fixture_for(SIM_FIXTURE, harness.board_info())


@pytest.fixture(scope="session")
def probe():
    return None


# DUT seconds the boot run gets before any case: as on a bench, which boots
# the DUT long before a case listens, what the sample does once is done
# before the judged runs.
BOOT_RUN_S = 1.0


@pytest.fixture(scope="session")
def sample(bench: SimBench, harness: SimHarness) -> SimSample:
    require_sample_image(bench.image)
    e = SimSample(bench)
    e.restart()
    harness.sleep(BOOT_RUN_S)
    return e


class SimDut(Dut):
    """The rig's own DUT image, answering on its RAM mailbox, which the rig
    reaches through the monitor instead of a probe. The image is in the
    DUT's flash from the bench's start; a reset is the machine's."""

    def __init__(self, bench: SimBench, harness: SimHarness, toolchain_dir: str):
        self.bench = bench
        self.harness = harness
        self.image = bench.image
        self.memory = MonitorMemory(bench)
        self.mailbox = Mailbox(self.memory, bench.image, toolchain_dir, None, DutClock(harness))

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
        pytest.skip("the sim platform has no RUN pin to press yet")


@pytest.fixture(scope="session")
def dut(bench: SimBench, harness: SimHarness, fixture: dict) -> SimDut:
    if example_metadata(bench.image) is not None:
        pytest.skip("the DUT image is a Zephyr sample, not a rig image")
    d = SimDut(bench, harness, _required("METALCLAW_ARM_TOOLCHAIN_DIR"))
    d.reset()
    d.ping()
    return d
