# SPDX-License-Identifier: Apache-2.0 OR MIT

from pathlib import Path
from types import SimpleNamespace

import pytest

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


def make_dut(probe):
    dut = object.__new__(Dut)
    dut.image = IMAGE
    dut.probe = probe
    dut.mailbox = SimpleNamespace(last_result=7)
    return dut


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
