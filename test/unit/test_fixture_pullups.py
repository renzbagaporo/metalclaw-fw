# SPDX-License-Identifier: Apache-2.0 OR MIT

from test.rig.platforms.hardware import platform
from test.rig.platforms.hardware.platform import FIXTURE, METALCLAW_PICO_RP2_FIXTURE, fixture_for


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
