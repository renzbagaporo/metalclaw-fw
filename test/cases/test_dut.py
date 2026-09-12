# SPDX-License-Identifier: Apache-2.0 OR MIT


def test_dut_answers_ping(dut):
    result = dut.ping()
    assert result["test"] == "test_ping"
    assert result["status"] == "ok"


def test_dut_reports_identity(dut):
    result = dut.command("ID")
    assert result["status"] == "ok"
    assert result["image"] == "smoke"


def test_dut_survives_reset(dut):
    dut.reset()
    result = dut.ping()
    assert result["status"] == "ok"
