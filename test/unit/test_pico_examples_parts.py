# SPDX-License-Identifier: Apache-2.0 OR MIT

import pytest

from test.pico_examples.parts import Attach, PinError, harness_pins, i2c_pins
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
