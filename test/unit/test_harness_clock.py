# SPDX-License-Identifier: Apache-2.0 OR MIT

import pytest

from test.rig.platforms.hardware import platform


class TickingHarness(platform.Harness):
    # A harness whose clock reads back the given ticks, one per read.
    def __init__(self, ticks):
        super().__init__("0123456789ABCDEF")
        self.ticks = list(ticks)

    def run(self, code, timeout=5.0):
        assert "ticks_us" in code
        return f"{self.ticks.pop(0)}\r\n"


def test_the_clock_counts_harness_ticks_across_their_wrap():
    period = platform.TICKS_PERIOD_US
    harness = TickingHarness([period - 500_000, period - 100_000, 300_000])
    assert harness.clock() == 0.0
    assert harness.clock() == pytest.approx(0.4)
    assert harness.clock() == pytest.approx(0.8)


def test_sleep_waits_until_the_harness_clock_has_moved_on(monkeypatch):
    harness = TickingHarness([0, 100_000, 400_000, 1_000_000])
    slept = []
    monkeypatch.setattr(platform.time, "sleep", slept.append)
    harness.sleep(1.0)
    assert slept == [1.0, pytest.approx(0.9), pytest.approx(0.6)]
    assert harness.ticks == []
