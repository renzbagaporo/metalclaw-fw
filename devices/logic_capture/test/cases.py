# SPDX-License-Identifier: Apache-2.0 OR MIT
# A DUT output (GP15, `GPIO line`) watched by the capture: every edge
# counted, each level timed.

import time

import pytest

NAME = "capture"
DUT_PIN = 15
WAIT_S = 10.0
# k_msleep's tick on the DUT, and the poll that reads the edge.
PULSE_TOLERANCE_NS = 2_000_000


def attach(fixture):
    pin = fixture["dut_pins"][DUT_PIN][0]
    return f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
metalclaw.attach('logic_capture', {NAME!r}, pin=({pin},))
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

    def wait_edges(self, count, start):
        deadline = time.monotonic() + WAIT_S
        while self.get("edges") - start < count and time.monotonic() < deadline:
            time.sleep(0.05)
        return self.get("edges") - start


def gpio(dut, args):
    result = dut.command(f"GPIO line {args}")
    assert result["test"] == "test_gpio" and result["status"] == "ok", result
    return result


@pytest.fixture(scope="module")
def capture(harness, dut, fixture):
    dut.require_image("logic_capture")
    harness.run(attach(fixture))
    dut.reset()
    dut.ping()
    gpio(dut, "SET 0")
    return Capture(harness)


def test_a_level_set_and_cleared_is_one_rise_and_one_fall(dut, capture):
    rising, falling, edges = capture.get("rising", "falling", "edges")
    gpio(dut, "SET 1")
    assert capture.wait_edges(1, edges) == 1
    assert capture.get("level", "rising", "falling") == (1, rising + 1, falling)
    gpio(dut, "SET 0")
    assert capture.wait_edges(2, edges) == 2
    assert capture.get("level", "rising", "falling") == (0, rising + 1, falling + 1)


def test_pulses_are_counted_and_timed(dut, capture):
    rising, edges = capture.get("rising", "edges")
    for _ in range(5):
        gpio(dut, "PULSE 20")
    assert capture.wait_edges(10, edges) == 10
    assert capture.get("rising") == rising + 5
    assert abs(capture.get("high_ns") - 20_000_000) <= PULSE_TOLERANCE_NS


def test_no_edge_was_missed(capture):
    assert capture.get("missed") == 0
