# SPDX-License-Identifier: Apache-2.0 OR MIT

from pathlib import Path
from types import SimpleNamespace

import pytest

from test.rig.platforms.hardware import platform
from test.rig.platforms.hardware.dap import DapError
from test.rig.platforms.hardware.platform import Dut
from test.rig.platforms.hardware.probe import ProbeError

IMAGE = Path("/images/smoke/zephyr.elf")


# A probe whose downloads fail with the given errors in turn, then succeed.
class FakeProbe:
    def __init__(self, *failures):
        self.failures = list(failures)
        self.calls = []

    def download(self, image, reset=True):
        self.calls.append(("download", image, reset))
        if self.failures:
            raise self.failures.pop(0)

    def reset(self):
        self.calls.append(("reset",))


# A DUT whose debug accesses answer WAIT `asleep` times, as a dormant one does.
class FakeMemory:
    def __init__(self, asleep=0):
        self.asleep = asleep
        self.writes = []

    def write_words(self, address, words):
        if self.asleep:
            self.asleep -= 1
            raise DapError("DAP transfer stopped after 0 of 1, ack 2")
        self.writes.append((address, words))


class FakeHarness:
    def __init__(self):
        self.code = []

    def run(self, code, timeout=5.0):
        self.code.append(code)
        return ""


def make_dut(probe, memory=None, harness=None):
    dut = object.__new__(Dut)
    dut.image = IMAGE
    dut.probe = probe
    dut.memory = memory or FakeMemory()
    dut.harness = harness or FakeHarness()
    dut.mailbox = SimpleNamespace(last_result=7)
    return dut


def test_a_dormant_dut_is_woken_on_gp15_and_halted_before_the_download():
    probe, memory, harness = FakeProbe(), FakeMemory(asleep=3), FakeHarness()
    dut = make_dut(probe, memory, harness)
    dut.flash(wake_harness_pin=15)
    assert memory.writes == [(platform.DHCSR, [platform.DHCSR_HALT])]
    assert "PWM(Pin(15)" in harness.code[0] and "_wake.deinit()" in harness.code[-1]
    assert probe.calls[0] == ("download", IMAGE, False)


def test_a_dut_that_never_wakes_raises_and_stops_the_square_wave(monkeypatch):
    monkeypatch.setattr(Dut, "WAKE_TIMEOUT_S", 0.0)
    harness = FakeHarness()
    dut = make_dut(FakeProbe(), FakeMemory(asleep=10**6), harness)
    with pytest.raises(DapError):
        dut.flash(wake_harness_pin=15)
    assert "_wake.deinit()" in harness.code[-1]


def test_a_download_that_succeeds_resets_once():
    probe = FakeProbe()
    dut = make_dut(probe)
    dut.flash()
    assert probe.calls == [("download", IMAGE, False), ("reset",)]
    assert dut.mailbox.last_result == 0


def test_a_failed_download_is_restored_and_still_raised():
    failure = ProbeError("Failed to erase flash sector at address 0x10004000")
    probe = FakeProbe(failure)
    dut = make_dut(probe)
    with pytest.raises(ProbeError) as raised:
        dut.flash()
    assert raised.value is failure
    assert any("downloaded again" in note and str(IMAGE) in note for note in raised.value.__notes__)
    assert probe.calls == [("download", IMAGE, False), ("download", IMAGE, False), ("reset",)]
    assert dut.mailbox.last_result == 0


def test_a_failed_restore_stops_the_session_with_both_errors():
    probe = FakeProbe(ProbeError("erase timed out"), ProbeError("attach failed"))
    dut = make_dut(probe)
    with pytest.raises(pytest.exit.Exception) as raised:
        dut.flash()
    message = str(raised.value)
    assert "erase timed out" in message and "attach failed" in message
    assert raised.value.returncode == 2
    assert probe.calls == [("download", IMAGE, False), ("download", IMAGE, False)]
