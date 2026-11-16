# SPDX-License-Identifier: Apache-2.0 OR MIT

from pathlib import Path

import pytest

from test.rig.platforms.emulator import bench, platform

pytest_plugins = ["pytester"]
ROOT = Path(__file__).resolve().parents[2]

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
    platform.record_diagnostic(pytestconfig, "console_bridge_example_console_overruns", 0)
    platform.record_diagnostic(pytestconfig, "console_bridge_stats", "unavailable: the REPL port is gone")
    platform._session["record"] = None
"""


def test_the_counts_reach_the_junit_properties_and_the_terminal_summary(pytester, monkeypatch):
    monkeypatch.setenv("PYTHONPATH", str(ROOT))
    pytester.makeconftest(PLUGIN)
    pytester.makepyfile("def test_passes(): pass\n")
    result = pytester.runpytest_subprocess("-p", "test.rig.platforms.emulator.platform", "--junitxml=junit.xml",
                                           "-o", "junit_family=xunit1")
    result.stdout.fnmatch_lines(["*- emulator diagnostics -*", "console_bridge_example_console_overruns: 0",
                                 "console_bridge_stats: unavailable: the REPL port is gone"])
    junit = (pytester.path / "junit.xml").read_text()
    assert '<property name="console_bridge_example_console_overruns" value="0"' in junit
    assert '<property name="console_bridge_stats" value="unavailable: the REPL port is gone"' in junit


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
