# SPDX-License-Identifier: Apache-2.0 OR MIT

from test.pico_examples import pins

TIMER, GPIO_IN, IO_BANK0 = pins.CHIPS["rp2350"]


class FakeLink:
    # A DUT whose GP25 toggles every 250 ms of its timer, which wraps.
    def __init__(self, step_us=1000, start=(1 << 32) - 600_000):
        self.now = start
        self.elapsed = 0
        self.step = step_us

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read_words(self, address, count):
        if address == TIMER:
            self.now = (self.now + self.step) % (1 << 32)
            self.elapsed += self.step
            return [self.now]
        t = self.elapsed
        if address == GPIO_IN:
            return [((t // 250_000) & 1) << 25]
        if address == IO_BANK0 + 8 * 25:
            return [pins.STATUS_OETOPAD]
        if IO_BANK0 <= address < IO_BANK0 + 8 * 48:
            return [0]
        raise AssertionError(hex(address))


def test_a_toggling_pin_is_seen_across_a_timer_wrap():
    samples = pins.watch(None, "rp2350", 2.0, (25,), link=FakeLink())
    assert samples[-1].us >= 2_000_000
    seen = pins.edges(samples, 25)
    assert 7 <= len(seen) <= 9
    assert pins.Toggle(25, 7, (0.24, 0.26)).check(samples) == []


def test_a_wrong_period_or_an_undriven_pin_is_reported():
    samples = pins.watch(None, "rp2350", 2.0, (7, 25), link=FakeLink())
    problems = pins.Toggle(25, 7, (0.45, 0.55)).check(samples)
    assert any("intervals outside" in p for p in problems)
    assert pins.Toggle(7, 1, (0.1, 1.0)).check(samples) != []


def test_an_interval_is_judged_within_what_coarse_samples_can_tell():
    # 70 ms between samples, as a slow emulated link gives: the 250 ms
    # toggles are seen up to 70 ms late, so single intervals read as short
    # as 0.18 s or as long as 0.32 s, and each still passes; a wrong period
    # still fails.
    samples = pins.watch(None, "rp2350", 3.0, (25,), link=FakeLink(step_us=70_000))
    seen = pins.edges(samples, 25)
    gaps = [b[0] - a[0] for a, b in zip(seen, seen[1:])]
    assert any(not 0.24 <= g <= 0.26 for g in gaps)
    assert pins.Toggle(25, 7, (0.24, 0.26)).check(samples) == []
    assert any("intervals outside" in p for p in pins.Toggle(25, 7, (0.45, 0.55)).check(samples))


def test_edge_bounds_hold_each_change_between_two_samples():
    samples = pins.watch(None, "rp2350", 1.0, (25,), link=FakeLink(step_us=70_000))
    for (lo, hi), (t, _) in zip(pins.edge_bounds(samples, 25), pins.edges(samples, 25)):
        assert lo < hi == t
        assert int(hi / 0.25) != int(lo / 0.25) or lo * 1e6 % 250_000 == 0
