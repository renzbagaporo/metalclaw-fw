# SPDX-License-Identifier: Apache-2.0 OR MIT

import pytest

from test.rig.platforms.hardware import platform

PROBE = "CMSIS-DAP v2"
BRIDGE = "Metalclaw DUT console"


class ReenumeratingHarness(platform.Harness):
    # A harness whose interface strings read back the given lists, one per
    # read, the last one for good.
    def __init__(self, reads, printed=""):
        super().__init__("0123456789ABCDEF")
        self.reads = list(reads)
        self.printed = printed
        self.enumerated = []

    def usb_interfaces(self):
        return self.reads.pop(0) if len(self.reads) > 1 else self.reads[0]

    def run(self, code, timeout=5.0):
        return self.printed

    def wait_enumerated(self, timeout=15.0):
        self.enumerated.append(timeout)


@pytest.fixture(autouse=True)
def no_waiting(monkeypatch):
    monkeypatch.setattr(platform.time, "sleep", lambda seconds: None)


def test_an_attach_waits_out_the_old_enumeration_and_the_gap():
    harness = ReenumeratingHarness([[PROBE], [PROBE], [], [PROBE, BRIDGE]])
    harness.wait_interface(BRIDGE, timeout=3.0)
    assert harness.reads == [[PROBE, BRIDGE]]
    assert harness.enumerated == [3.0]


def test_a_detach_is_not_done_while_the_harness_is_off_usb():
    harness = ReenumeratingHarness([[PROBE, BRIDGE], [], [], [PROBE]])
    harness.wait_interface(BRIDGE, present=False)
    assert harness.reads == [[PROBE]]
    assert harness.enumerated == [15.0]


def test_an_interface_that_never_comes_times_out(monkeypatch):
    now = iter(range(100))
    monkeypatch.setattr(platform.time, "monotonic", lambda: next(now))
    harness = ReenumeratingHarness([[PROBE]])
    with pytest.raises(TimeoutError, match="did not enumerate with 'Metalclaw DUT console'"):
        harness.wait_interface(BRIDGE, timeout=5.0)
    assert harness.enumerated == []


def test_detaching_a_usb_device_waits_for_the_strings_to_change():
    harness = ReenumeratingHarness([[PROBE, BRIDGE], [PROBE, BRIDGE], [], [PROBE]], printed="usb\r\n")
    platform.detach_all_but_probe(harness)
    assert harness.reads == [[PROBE]]
    assert len(harness.enumerated) == 1


def test_detaching_no_usb_device_does_not_wait():
    harness = ReenumeratingHarness([[PROBE], []])
    platform.detach_all_but_probe(harness)
    assert harness.reads == [[]]
    assert harness.enumerated == []
