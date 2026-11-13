# SPDX-License-Identifier: Apache-2.0 OR MIT

import pytest

from test.rig.platforms.emulator import platform


@pytest.fixture(autouse=True)
def no_bench_environment(monkeypatch):
    for name in (platform.HARNESS_BOARD_ENV, "METALCLAW_HARNESS_SERIAL"):
        monkeypatch.delenv(name, raising=False)


def test_the_bench_defaults_to_fixture_1(monkeypatch):
    assert platform.layout() == platform.LAYOUTS["rpi_pi_pico"]
    assert platform._serial() == "0123456789ABCDEF"


def test_the_harness_board_picks_the_carrier_bench(monkeypatch):
    monkeypatch.setenv(platform.HARNESS_BOARD_ENV, "metalclaw_pico_rp2")
    assert platform.layout().dut_chip == "rp2040"
    assert platform._serial() == "00000000C0FFEE01"


def test_a_harness_board_without_a_bench_stops_the_run(monkeypatch):
    monkeypatch.setenv(platform.HARNESS_BOARD_ENV, "fixture1_pico")
    with pytest.raises(pytest.exit.Exception, match="no emulated bench for harness board 'fixture1_pico'"):
        platform.layout()


def test_a_bench_harness_serial_in_the_environment_leaves_the_emulated_one(monkeypatch):
    monkeypatch.setenv(platform.HARNESS_BOARD_ENV, "metalclaw_pico_rp2")
    monkeypatch.setenv("METALCLAW_HARNESS_SERIAL", "D4D2E0179F4FECF3")
    assert platform._serial() == "00000000C0FFEE01"
