# SPDX-License-Identifier: Apache-2.0 OR MIT
# The DUT's PWM (Zephyr's pwm_set_dt on slice 7 A, GP14) timed by the
# capture: what it reads against what the DUT's slice makes, which the
# DUT's answer gives in ns.

import time

import pytest

NAME = "capture"
DUT_PIN = 14
# Two harness cycles at each edge, and a DUT tick on top; and the two
# crystals, each within tens of ppm, apart.
TOLERANCE_NS = 50
TOLERANCE_PPM = 100
# Edges a second the engine keeps up with between polls (README): a 10 kHz
# PWM loses some, though every cycle it times is right.
KEEPS_UP_HZ = 3_000
WAIT_S = 20.0


def attach(fixture):
    pin = fixture["dut_pins"][DUT_PIN][0]
    return f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
metalclaw.attach('pwm_capture', {NAME!r}, pin=({pin},))
"""


SELECT = f"""import metalclaw
capture = [i for i in metalclaw.instances() if i.name == {NAME!r}][0]
"""


class Capture:
    def __init__(self, harness):
        self.harness = harness

    def get(self, *names):
        got = eval(self.harness.run(SELECT + f"print(({', '.join(f'capture.{n}' for n in names)},))\n").strip())
        return got if len(names) > 1 else got[0]

    def wait(self, test, what, timeout=WAIT_S):
        deadline = time.monotonic() + timeout
        while True:
            got = test(self)
            if got or time.monotonic() > deadline:
                assert got, what
                return
            time.sleep(0.05)

    def wait_periods(self, count):
        start = self.get("periods")
        self.wait(lambda c: c.get("periods") - start >= count, f"{count} periods")


@pytest.fixture(scope="module")
def capture(harness, dut, fixture):
    dut.require_image("pwm_capture")
    harness.run(attach(fixture))
    dut.reset()
    dut.ping()
    return Capture(harness)


def drive(dut, period_ns, pulse_ns):
    result = dut.command(f"PWM out {period_ns} {pulse_ns}")
    assert result["test"] == "test_pwm" and result["status"] == "ok", result
    return result["period_ns"], result["pulse_ns"]


def near(got, want):
    return abs(got - want) <= TOLERANCE_NS + want * TOLERANCE_PPM // 1_000_000


# A servo frame, the 1 kHz of a motor's PWM, both edges near each other, and
# 10 kHz.
@pytest.mark.parametrize("period_ns, pulse_ns", [
    (20_000_000, 1_500_000), (1_000_000, 300_000), (1_000_000, 750_000), (1_000_000, 10_000), (100_000, 25_000),
])
def test_the_period_and_pulse_are_what_the_dut_drives(dut, capture, period_ns, pulse_ns):
    missed = capture.get("missed")
    period, pulse = drive(dut, period_ns, pulse_ns)
    capture.wait_periods(3)
    got_period, got_pulse, duty, still = capture.get("period_ns", "pulse_ns", "duty_ppm", "still")
    assert near(got_period, period) and near(got_pulse, pulse), (got_period, got_pulse, period, pulse)
    assert abs(duty - pulse * 1_000_000 // period) <= 1_000 and not still
    if 1_000_000_000 // period_ns <= KEEPS_UP_HZ:
        assert capture.get("missed") == missed


def test_a_new_pulse_shows_within_two_periods(dut, capture):
    # The slice takes a new level at its next wrap, before the DUT answers:
    # two whole cycles counted after the answer are both at the new pulse.
    drive(dut, 20_000_000, 1_500_000)
    capture.wait_periods(3)
    _, pulse = drive(dut, 20_000_000, 1_000_000)
    capture.wait_periods(2)
    assert near(capture.get("pulse_ns"), pulse)


@pytest.mark.parametrize("level", [0, 1])
def test_a_held_output_reads_0_or_100_percent(dut, capture, level):
    drive(dut, 1_000_000, 500_000)
    capture.wait_periods(3)
    drive(dut, 1_000_000, 1_000_000 if level else 0)
    capture.wait(lambda c: c.get("still"), "the line held")
    assert capture.get("level", "duty_ppm") == (level, 1_000_000 * level)

