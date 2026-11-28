# SPDX-License-Identifier: Apache-2.0 OR MIT
# The driver's inputs from the DUT's PWM (1A GP12, 2A GP13, 1,2EN GP14):
# the outputs by the function table and the pair's drive by the duties,
# for the ways a DUT drives a motor through it. 3A, 4A and 3,4EN are not
# wired and read high.

import time

import pytest

NAME = "bridge"
DUT_PINS = {"a1": 12, "a2": 13, "en12": 14}
PERIOD_NS = 1_000_000
WAIT_S = 20.0
# 0.1 % of full drive: two harness cycles at each edge of 1 ms cycles.
TOLERANCE_PPM = 1_000


def attach(fixture):
    pins = ", ".join(f"{name}=({fixture['dut_pins'][pin][0]},)" for name, pin in DUT_PINS.items())
    return f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
metalclaw.attach('sn754410', {NAME!r}, {pins})
"""


SELECT = f"""import metalclaw
bridge = [i for i in metalclaw.instances() if i.name == {NAME!r}][0]
"""


class Bridge:
    def __init__(self, harness):
        self.harness = harness

    def get(self, name):
        return eval(self.harness.run(SELECT + f"print(bridge.{name})\n").strip())

    def wait_drive(self, want):
        deadline = time.monotonic() + WAIT_S
        while abs(self.get("drive_ppm")[0] - want) > TOLERANCE_PPM and time.monotonic() < deadline:
            time.sleep(0.05)
        return self.get("drive_ppm")[0]


def drive(dut, name, duty_percent):
    result = dut.command(f"PWM {name} {PERIOD_NS} {PERIOD_NS * duty_percent // 100}")
    assert result["test"] == "test_pwm" and result["status"] == "ok", result


def inputs(dut, a1, a2, en12):
    for name, duty in (("a1", a1), ("a2", a2), ("en12", en12)):
        drive(dut, name, duty)


@pytest.fixture(scope="module")
def bridge(harness, dut, fixture):
    dut.require_image("sn754410")
    harness.run(attach(fixture))
    dut.reset()
    dut.ping()
    return Bridge(harness)


# Sign-magnitude on the enable, the same on 2A with the enable held, and
# locked anti-phase (both A switching).
@pytest.mark.parametrize("a1, a2, en12, drive_ppm", [
    (100, 0, 40, 400_000), (0, 25, 100, -250_000), (70, 30, 100, 400_000), (100, 100, 100, 0),
])
def test_the_pair_s_drive_follows_the_duties(dut, bridge, a1, a2, en12, drive_ppm):
    inputs(dut, a1, a2, en12)
    assert abs(bridge.wait_drive(drive_ppm) - drive_ppm) <= TOLERANCE_PPM
    duties = bridge.get("duty_ppm")
    for got, want in zip(duties[:3], (a1, a2, en12)):
        assert abs(got - want * 10_000) <= TOLERANCE_PPM, duties


def test_the_outputs_follow_the_function_table(dut, bridge):
    # 9.4 Table 1: EN high passes A through; EN low turns both off.
    inputs(dut, 100, 0, 100)
    bridge.wait_drive(1_000_000)
    assert bridge.get("outputs") == (1, 0, 1, 1)
    inputs(dut, 100, 0, 0)
    bridge.wait_drive(0)
    assert bridge.get("outputs") == (2, 2, 1, 1)


def test_no_edge_was_missed(bridge):
    assert bridge.get("missed") == 0
