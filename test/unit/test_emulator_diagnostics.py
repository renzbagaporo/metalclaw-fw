# SPDX-License-Identifier: Apache-2.0 OR MIT

import socket
import subprocess
import threading
from pathlib import Path

import pytest

from test.rig.platforms.emulator import bench, platform

pytest_plugins = ["pytester"]
ROOT = Path(__file__).resolve().parents[2]

# Renode 1.17.0's monitor, as it answered on a bench's port.
GREETING = (b'\xff\xfd\x00\xff\xfd\x1f\xff\xfb\x01\xff\xfb\x03\xff\xfc"Renode, version 1.17.0\n\r\n\r'
            b'(monitor) include @bench.resc\n\r(pico2) ')
REPLIES = {
    "uart0_dut_tx LateDeliveries": b"uart0_dut_tx LateDeliveries\n\r0x0000000000000003\r\r\n(pico2) ",
    "uart0_harness_tx LateDeliveries":
        b"uart0_harness_tx LateDeliveries\n\rNo such command or device: uart0_harness_tx\r\n(pico2) ",
}
NO_PROPERTY = ("uart0_dut_tx LateDeliveries\n\rThere was an error executing command 'uart0_dut_tx "
               "LateDeliveries': \r\nuart0_dut_tx does not provide a field, method or property "
               "LateDeliveries.\r\n(monitor) ")


class FakeMonitor(threading.Thread):
    def __init__(self, replies=REPLIES, greeting=GREETING):
        super().__init__(daemon=True)
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.replies = replies
        self.greeting = greeting
        self.commands = []

    def run(self):
        connection, _ = self.server.accept()
        with connection:
            # The greeting in two parts, as a bench that is still printing
            # sends it.
            connection.sendall(self.greeting[:40])
            connection.sendall(self.greeting[40:])
            buffer = b""
            while True:
                data = connection.recv(4096)
                if not data:
                    return
                buffer += data
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    command = line.decode()
                    self.commands.append(command)
                    if command in self.replies:
                        connection.sendall(self.replies[command])


def test_a_counter_reads_as_the_number_the_monitor_printed():
    reply = bench._monitor_text(REPLIES["uart0_dut_tx LateDeliveries"])
    assert bench.monitor_value(reply, "uart0_dut_tx LateDeliveries") == 3


@pytest.mark.parametrize("reply", [REPLIES["uart0_harness_tx LateDeliveries"], NO_PROPERTY.encode()],
                         ids=["no wire", "no counter"])
def test_a_package_without_the_wire_or_its_counter_reads_as_none(reply):
    command = bench._monitor_text(reply).split("\n", 1)[0]
    assert bench.monitor_value(bench._monitor_text(reply), command) is None


def test_the_bench_reads_each_uart0_wires_late_deliveries_before_it_quits(tmp_path):
    monitor = FakeMonitor()
    monitor.start()
    b = bench.Bench(tmp_path, bench.LAYOUTS["rpi_pi_pico"], tmp_path / "h.uf2", tmp_path, tmp_path / "z.elf",
                    tmp_path / "work", "0123456789ABCDEF")
    b.process = subprocess.Popen(["sleep", "30"])
    b.monitor_port = monitor.port
    try:
        assert b.late_deliveries() == {"uart0_dut_tx": 3, "uart0_harness_tx": None}
    finally:
        b.process.kill()
        b.process.wait()
    assert monitor.commands == ["uart0_dut_tx LateDeliveries", "uart0_harness_tx LateDeliveries"]


def test_a_monitor_that_never_answers_reads_as_none(monkeypatch):
    monitor = FakeMonitor(replies={})
    monitor.start()
    assert bench.monitor_replies(monitor.port, ["uart0_dut_tx LateDeliveries"], timeout=1.0) == [None]


def test_a_bench_that_is_gone_is_not_asked(tmp_path):
    b = bench.Bench(tmp_path, bench.LAYOUTS["rpi_pi_pico"], tmp_path / "h.uf2", tmp_path, tmp_path / "z.elf",
                    tmp_path / "work", "0123456789ABCDEF")
    b.process = subprocess.Popen(["sh", "-c", "exit 0"])
    b.process.wait()
    b.monitor_port = 1
    assert b.late_deliveries() == {"uart0_dut_tx": None, "uart0_harness_tx": None}


class FakeHarness:
    def __init__(self, answer):
        self.answer = answer
        self.calls = []

    def run(self, code, timeout=5.0):
        self.calls.append((code, timeout))
        if isinstance(self.answer, Exception):
            raise self.answer
        return self.answer


class FakeConfig:
    def __init__(self):
        self.stash = pytest.Stash()


def test_each_console_bridges_uart_counters_are_recorded(monkeypatch):
    recorded = []
    monkeypatch.setitem(platform._session, "record", lambda name, value: recorded.append((name, value)))
    config = FakeConfig()
    harness = FakeHarness("example_console frame_errors=2 overruns=5 fifo_overruns=1 ring_overruns=4 "
                          "short_writes=3\n")
    platform.record_console_stats(config, harness)
    assert recorded == [("console_bridge_example_console_frame_errors", "2"),
                        ("console_bridge_example_console_overruns", "5"),
                        ("console_bridge_example_console_fifo_overruns", "1"),
                        ("console_bridge_example_console_ring_overruns", "4"),
                        ("console_bridge_example_console_short_writes", "3")]
    assert config.stash[platform.DIAGNOSTICS] == recorded
    assert harness.calls[0][1] == platform.CONSOLE_STATS_TIMEOUT_S


def test_a_harness_that_cannot_answer_is_recorded_as_such(monkeypatch):
    monkeypatch.setitem(platform._session, "record", None)
    config = FakeConfig()
    platform.record_console_stats(config, FakeHarness(RuntimeError("harness 0123 REPL port is gone")))
    assert config.stash[platform.DIAGNOSTICS] == [
        ("console_bridge_stats", "unavailable: harness 0123 REPL port is gone")]


PLUGIN = """
import pytest
from test.rig.platforms.emulator import platform


@pytest.fixture(scope="session", autouse=True)
def diagnostics(pytestconfig, record_testsuite_property):
    platform._session["record"] = record_testsuite_property
    yield
    platform.record_late_deliveries(pytestconfig, {"uart0_dut_tx": 0, "uart0_harness_tx": None})
    platform._session["record"] = None
"""


def test_the_counts_reach_the_junit_properties_and_the_terminal_summary(pytester, monkeypatch):
    monkeypatch.setenv("PYTHONPATH", str(ROOT))
    pytester.makeconftest(PLUGIN)
    pytester.makepyfile("def test_passes(): pass\n")
    result = pytester.runpytest_subprocess("-p", "test.rig.platforms.emulator.platform", "--junitxml=junit.xml",
                                           "-o", "junit_family=xunit1")
    result.stdout.fnmatch_lines(["*- emulator diagnostics -*", "renode_uart0_dut_tx_late_deliveries: 0",
                                 "renode_uart0_harness_tx_late_deliveries: absent from this Renode package"])
    junit = (pytester.path / "junit.xml").read_text()
    assert '<property name="renode_uart0_dut_tx_late_deliveries" value="0"' in junit
    assert '<property name="renode_uart0_harness_tx_late_deliveries" value="absent from this Renode package"' \
        in junit


CONSOLE_PLUGIN = """
import pytest
from test.rig.platforms.emulator import platform

EVENTS = []


class Harness:
    def run(self, code, timeout=5.0):
        EVENTS.append("stats read")
        return "example_console frame_errors=0 overruns=7\\n"


@pytest.fixture(scope="session", autouse=True)
def session(record_testsuite_property):
    platform._session["record"] = record_testsuite_property
    platform._session["harness"] = Harness()
    yield
    open("events.txt", "w").write(" ".join(EVENTS))


@pytest.fixture(scope="module")
def console():
    EVENTS.append("attached")
    yield
    EVENTS.append("detached")
"""


def test_a_programs_console_counters_are_read_before_its_bridge_is_detached(pytester, monkeypatch):
    monkeypatch.setenv("PYTHONPATH", str(ROOT))
    pytester.makeconftest(CONSOLE_PLUGIN)
    pytester.makepyfile(test_program="def test_console(console): pass\ndef test_parts(): pass\n",
                        test_other="def test_alone(): pass\n")
    result = pytester.runpytest_subprocess("-p", "test.rig.platforms.emulator.platform")
    result.stdout.fnmatch_lines(["console_bridge_example_console_frame_errors: 0",
                                 "console_bridge_example_console_overruns: 7"])
    assert (pytester.path / "events.txt").read_text() == "attached stats read detached"
