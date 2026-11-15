# SPDX-License-Identifier: Apache-2.0 OR MIT

import re
from pathlib import Path

import pytest

from test.pico_examples.part_targets import PART_SPECS
from test.pico_examples.parts import PARTS, Attach, PinError, harness_pins, i2c_pins
from test.rig.platforms.hardware.platform import FIXTURE, METALCLAW_PICO_RP2_FIXTURE


def test_fixture_1_is_pin_for_pin_but_the_crossed_uart():
    assert harness_pins(FIXTURE, (4, 5)) == (4, 5)
    assert harness_pins(FIXTURE, (0, 1)) == (1, 0)


def test_the_probes_pins_reach_nothing_on_fixture_1():
    with pytest.raises(PinError):
        harness_pins(FIXTURE, (2,))


def test_a_carrier_binding_takes_branches_from_one_pio_window():
    # SPI0 defaults: GP16 RX, GP17 CSn, GP18 SCK, GP19 TX. GP18 has only
    # GPIO38, so all four come from the 16-47 window.
    got = harness_pins(METALCLAW_PICO_RP2_FIXTURE, (18, 19, 16, 17))
    assert got == (38, 32, 35, 34)
    assert all(16 <= p < 48 for p in got)


def test_the_carrier_i2c_default_pair_is_the_rigs_own():
    assert harness_pins(METALCLAW_PICO_RP2_FIXTURE, (4, 5), i2c_pins) == (12, 13)


def test_an_attach_line_carries_harness_pins_then_values():
    a = Attach("bmp280", "baro", {"i2c": (4, 5, 0x76)}, {"i2c": 2}, {"pressure": 90000})
    assert a.code(FIXTURE) == ("import metalclaw\nbaro = metalclaw.attach('bmp280', 'baro', i2c=(4, 5, 118))\n"
                               "baro.pressure = 90000\n")


def test_a_carrier_i2c_attach_takes_the_branches_with_i2c_functions():
    a = Attach("bmp280", "baro", {"i2c": (4, 5, 0x76)}, {"i2c": 2})
    assert "i2c=(12, 13, 118)" in a.code(METALCLAW_PICO_RP2_FIXTURE)


def test_a_pair_without_i2c_functions_is_refused():
    with pytest.raises(PinError):
        harness_pins(FIXTURE, (5, 6), i2c_pins)


def test_every_part_attaches_on_both_fixtures():
    for target, parts in PARTS.items():
        for part in parts:
            for fixture in (FIXTURE, METALCLAW_PICO_RP2_FIXTURE):
                assert f"metalclaw.attach({part.model!r}, {part.name!r}, " in part.code(fixture), target


def test_the_spi_part_takes_the_carrier_window_branches():
    (env,) = PARTS["bme280_spi"]
    assert "spi=(38, 32, 35, 34)" in env.code(METALCLAW_PICO_RP2_FIXTURE)
    assert "spi=(18, 19, 16, 17)" in env.code(FIXTURE)


# A program that prints nothing is judged by its parts' checks alone.
def test_every_attached_model_is_a_device_and_every_target_has_criteria():
    root = Path(__file__).resolve().parents[2]
    models = set()
    for cmake in (root / "devices").glob("*/device.cmake"):
        models |= set(re.findall(r"metalclaw_device\((\w+)", cmake.read_text()))
    for target, parts in PARTS.items():
        assert {p.model for p in parts} <= models, target
        assert (target in PART_SPECS and PART_SPECS[target].source) or any(p.checks for p in parts), target


def test_a_part_carries_its_checks():
    from test.pico_examples.parts import Check
    c = Check("part.frames", lambda v: v > 0, "the program drew a frame")
    a = Attach("ssd1306", "oled", {"i2c": (4, 5, 0x3C)}, {"i2c": 2}, checks=(c,))
    assert a.checks[0].test(3) and not a.checks[0].test(0)
