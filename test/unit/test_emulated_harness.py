# SPDX-License-Identifier: Apache-2.0 OR MIT

import inspect

import pytest

from test.rig.platforms.emulator import platform


def test_the_emulated_harness_waits_for_serial_ports_as_long_as_for_enumeration():
    default = inspect.signature(platform.EmulatedHarness.serial_port).parameters["timeout"].default
    assert default == platform.ENUMERATION_TIMEOUT_S


class FakeMailbox:
    def __init__(self):
        self.calls = []

    def wait_ready(self, timeout):
        self.calls.append(("wait_ready", timeout))

    def ping(self, timeout):
        self.calls.append(("ping", timeout))
        return {}

    def command(self, line, timeout):
        self.calls.append(("command", timeout))
        return {}

    def send(self, line, timeout):
        self.calls.append(("send", timeout))
        return 1

    def receive(self, sequence, timeout):
        self.calls.append(("receive", timeout))
        return {}


def emulated_dut(monkeypatch, scale):
    monkeypatch.delenv("METALCLAW_DUT_MAILBOX_TIMEOUT_S", raising=False)
    dut = platform.EmulatedDut.__new__(platform.EmulatedDut)
    dut.mailbox = FakeMailbox()
    dut.scale = scale
    return dut


def test_the_emulated_dut_scales_every_mailbox_deadline(monkeypatch):
    dut = emulated_dut(monkeypatch, 60.0)
    dut.ping()
    dut.command("ID", 30.0)
    dut.finish(dut.start("RX"))
    assert dut.mailbox.calls == [("wait_ready", 600.0), ("ping", 600.0), ("command", 1800.0),
                                 ("send", 600.0), ("receive", 600.0)]


def test_the_time_scale_comes_from_the_environment(monkeypatch):
    monkeypatch.setenv(platform.TIME_SCALE_ENV, "2.5")
    assert platform._time_scale() == 2.5
    monkeypatch.delenv(platform.TIME_SCALE_ENV)
    assert platform._time_scale() == platform.TIME_SCALE


@pytest.mark.parametrize("value", ["0", "-1", "nan", "inf", "fast"])
def test_an_invalid_time_scale_is_refused(monkeypatch, value):
    monkeypatch.setenv(platform.TIME_SCALE_ENV, value)
    with pytest.raises(ValueError, match="positive finite factor"):
        platform._time_scale()


def test_the_emulated_harness_scales_repl_deadlines(monkeypatch):
    seen = []
    monkeypatch.setattr(platform.Harness, "run", lambda self, code, timeout=5.0: seen.append(timeout) or "")
    monkeypatch.setenv(platform.TIME_SCALE_ENV, "4")
    platform.EmulatedHarness("0123456789ABCDEF").run("pass", 2.5)
    assert seen == [10.0]
