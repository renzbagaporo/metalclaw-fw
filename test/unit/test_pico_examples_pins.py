# SPDX-License-Identifier: Apache-2.0 OR MIT

from test.pico_examples import pins

TIMER, GPIO_IN, IO_BANK0 = pins.CHIPS["rp2350"]
PADS = pins.PADS["rp2350"]


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
        if PADS <= address < PADS + 4 * 48:
            return [pins.PAD_IE]
        raise AssertionError(hex(address))


class OutputOnlyLink(FakeLink):
    # GP25 toggling with its pad input off, as Zephyr leaves an output-only
    # pin: GPIO_IN reads it 0, and only OUTTOPAD shows the level.
    def read_words(self, address, count):
        if address == PADS + 4 * 25:
            return [0]
        if address == GPIO_IN:
            return [0]
        if address == IO_BANK0 + 8 * 25:
            return [pins.STATUS_OETOPAD | (((self.elapsed // 250_000) & 1) * pins.STATUS_OUTTOPAD)]
        return super().read_words(address, count)


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


def test_a_claim_before_the_first_toggle_is_judged_only_when_asked():
    # pio_blink on a pulled-up line: the claim drives GP6 low 7.4 ms in, the
    # first toggle comes 2.4 ms later, then 0.5 s halves.
    times = [0.0074, 0.0098, 0.5098, 1.0098, 1.5098]
    samples, level = [pins.Sample(0, 1 << 6, 1 << 6)], 1
    for t in times:
        level ^= 1
        samples.append(pins.Sample(round(t * 1e6), level << 6, 1 << 6))
    samples.append(pins.Sample(2_000_000, level << 6, 1 << 6))
    assert any("intervals outside" in p for p in pins.Toggle(6, 4, (0.45, 0.55)).check(samples))
    assert pins.Toggle(6, 4, (0.45, 0.55), judged_from=1).check(samples) == []


class RestartingLink(FakeLink):
    # A DUT whose timer starts again from 0 a few reads in, as a reset that
    # lands after the watch began.
    def __init__(self):
        super().__init__(step_us=1000, start=5_000_000)
        self.reads = 0

    def read_words(self, address, count):
        if address == TIMER:
            self.reads += 1
            if self.reads == 4:
                self.now, self.elapsed = 0, 0
                return [0]
        return super().read_words(address, count)


def test_a_dut_that_restarts_under_the_watch_is_watched_from_its_restart():
    samples = pins.watch(None, "rp2350", 1.0, (25,), link=RestartingLink())
    assert samples[0].us < 2000 and 0.99e6 <= samples[-1].us - samples[0].us < 1.01e6
    assert all(b.us > a.us for a, b in zip(samples, samples[1:]))


def test_an_output_only_pin_is_read_from_what_drives_it():
    samples = pins.watch(None, "rp2350", 2.0, (25,), link=OutputOnlyLink())
    assert 7 <= len(pins.edges(samples, 25)) <= 9


def test_a_count_steps_up_by_its_programs_own_table():
    from test.zephyr_samples.pin_specs import GPIO_7SEG_DIGITS
    shown = [GPIO_7SEG_DIGITS[d % 10] << 2 for d in range(7, 13)]
    samples = [pins.Sample(i * 10_000, shown[i // 10], 0) for i in range(60)]
    assert pins.Count(2, 5, GPIO_7SEG_DIGITS).check(samples) == []
    assert pins.Count(2, 5).check(samples) != []
    assert pins.Count(2, 5, GPIO_7SEG_DIGITS).pins == (2, 3, 4, 5, 6, 7, 8)
    backwards = list(reversed(samples))
    assert pins.Count(2, 1, GPIO_7SEG_DIGITS).check(backwards)


def test_a_count_passes_over_a_sample_read_across_a_change():
    from test.zephyr_samples.pin_specs import GPIO_7SEG_DIGITS
    zero, one = GPIO_7SEG_DIGITS[0] << 2, GPIO_7SEG_DIGITS[1] << 2
    torn = GPIO_7SEG_DIGITS[7] << 2  # A-C of the old 0, D-F of the new 1
    levels = [zero] * 10 + [torn] + [one] * 10
    samples = [pins.Sample(i * 4_000, v, 0) for i, v in enumerate(levels)]
    assert pins.Count(2, 1, GPIO_7SEG_DIGITS).check(samples) == []


def test_a_count_from_a_digit_leaves_out_what_came_before_it():
    # auxdisplay_digits: all segments (an 8), blank, then 0, 1, 2, ...
    from test.zephyr_samples.pin_specs import GPIO_7SEG_DIGITS
    shown = [GPIO_7SEG_DIGITS[8] << 2, 0, *(GPIO_7SEG_DIGITS[d % 10] << 2 for d in range(0, 8))]
    samples = [pins.Sample(i * 10_000, shown[i // 10], 0) for i in range(100)]
    assert pins.Count(2, 5, GPIO_7SEG_DIGITS).check(samples) != []
    assert pins.Count(2, 5, GPIO_7SEG_DIGITS, from_digit=0).check(samples) == []



class SteppedLink(FakeLink):
    # The native platform's monitor: the DUT stands still unless advanced,
    # and the watch holds it from the first read to the last.
    def __init__(self):
        super().__init__(step_us=0)
        self.calls = []

    def hold(self):
        self.calls.append("hold")

    def advance(self, seconds):
        self.calls.append("advance")
        self.now = (self.now + int(seconds * 1e6)) % (1 << 32)
        self.elapsed += int(seconds * 1e6)

    def release(self):
        self.calls.append("release")


def test_a_link_that_steps_the_dut_is_sampled_at_even_dut_times():
    link = SteppedLink()
    samples = pins.watch(None, "rp2350", 1.0, (25,), link=link)
    gaps = {b.us - a.us for a, b in zip(samples, samples[1:])}
    assert gaps == {int(pins.STEP_S * 1e6)}
    assert link.calls[0] == "hold" and link.calls[-1] == "release"
    assert len(pins.edges(samples, 25)) == 4
