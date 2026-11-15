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
