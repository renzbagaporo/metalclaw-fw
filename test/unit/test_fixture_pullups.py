# SPDX-License-Identifier: Apache-2.0 OR MIT

import types

from test.pico_examples.parts import parts_dut_pins
from test.rig.platforms.hardware import platform
from test.rig.platforms.hardware.platform import FIXTURE, METALCLAW_PICO_RP2_FIXTURE, fixture_for, idle_levels


class ReportingHarness(platform.Harness):
    # A harness whose REPL answers with a fixed metalclaw.board() report.
    def __init__(self, report):
        self.report = report
        self.code = []

    def run(self, code, timeout=5.0):
        self.code.append(code)
        return repr(self.report) + "\r\n"


def test_a_board_with_line_pullups_holds_its_idle_levels():
    f = fixture_for(METALCLAW_PICO_RP2_FIXTURE, {"line_pullups": True})
    assert f["line_pullups"] is True
    assert f["idle"] == {22: 0}
    assert f["swd"] == METALCLAW_PICO_RP2_FIXTURE["swd"]


def test_a_board_without_line_pullups_holds_no_idle_levels():
    f = fixture_for(METALCLAW_PICO_RP2_FIXTURE, {"line_pullups": False})
    assert f["line_pullups"] is False
    assert f["idle"] == {}
    assert f["spi"] == METALCLAW_PICO_RP2_FIXTURE["spi"]


def test_a_pin_map_without_idle_levels_gets_an_empty_one():
    assert fixture_for(FIXTURE, {"line_pullups": True})["idle"] == {}
    assert fixture_for(FIXTURE, {"line_pullups": False})["idle"] == {}


def test_the_pin_map_is_left_as_it_was():
    fixture_for(METALCLAW_PICO_RP2_FIXTURE, {"line_pullups": False})
    assert METALCLAW_PICO_RP2_FIXTURE["idle"] == {22: 0}
    assert "line_pullups" not in METALCLAW_PICO_RP2_FIXTURE


def test_the_harness_reads_its_board_report():
    for report in ({"line_pullups": True}, {"line_pullups": False}):
        h = ReportingHarness(report)
        assert h.board_info() == report
        assert "metalclaw.board()" in h.code[0]


def test_idle_levels_are_driven_from_the_harness():
    h = ReportingHarness({})
    platform.hold_idle(h, {"idle": {22: 0, 7: 1}})
    assert h.code == ["from machine import Pin\nPin(22, Pin.OUT, value=0)\nPin(7, Pin.OUT, value=1)\n"]


def test_no_idle_levels_send_nothing_to_the_harness():
    h = ReportingHarness({})
    platform.hold_idle(h, {"idle": {}})
    assert h.code == []


CARRIER = fixture_for(METALCLAW_PICO_RP2_FIXTURE, {"line_pullups": True})


def test_an_idle_level_yields_to_a_net_a_part_takes():
    # GPIO22 is DUT GP10's one branch; pio_hub75 drives GP10 as its E line.
    assert 10 in parts_dut_pins("pio_hub75", "pico")
    assert idle_levels(CARRIER, parts_dut_pins("pio_hub75", "pico")) == {}
    h = ReportingHarness({})
    platform.hold_idle(h, CARRIER, parts_dut_pins("pio_hub75", "pico"))
    assert h.code == []


def test_an_idle_level_yields_by_net_not_by_branch():
    # DUT GP11's branches are GPIO16 and GPIO23: a level held on one yields
    # when a part takes the net through the other.
    fixture = {**CARRIER, "idle": {16: 0, 22: 0}}
    assert idle_levels(fixture, {11}) == {22: 0}
    assert idle_levels(fixture, {10, 11}) == {}


def test_an_idle_level_stays_for_a_program_whose_parts_leave_its_net():
    assert idle_levels(CARRIER, parts_dut_pins("bmp280_i2c", "pico")) == {22: 0}
    assert idle_levels(CARRIER) == {22: 0}
    h = ReportingHarness({})
    platform.hold_idle(h, CARRIER, parts_dut_pins("ssd1306_i2c", "pico"))
    assert h.code == ["from machine import Pin\nPin(22, Pin.OUT, value=0)\n"]


def test_an_example_names_the_pins_its_build_s_parts_take():
    # The RTC clock on GP20 is for the pico build of low_power_dormant_timer only.
    def example(board):
        return types.SimpleNamespace(target="low_power_dormant_timer", metadata={"board": board})
    assert platform.Example.parts_dut_pins(example("pico")) == {20}
    assert platform.Example.parts_dut_pins(example("pico2")) == set()
    assert platform.Example.parts_dut_pins(types.SimpleNamespace(target="pio_hub75", metadata={})) >= {10}
