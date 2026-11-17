# SPDX-License-Identifier: Apache-2.0 OR MIT

import types

import pytest

from test.rig.platforms.hardware import dap

from test.rig.platforms.hardware.dap import USB_TIMEOUT_ENV, USB_TIMEOUT_MS, DapError, usb_timeout_ms


def test_usb_timeout_defaults_without_the_environment(monkeypatch):
    monkeypatch.delenv(USB_TIMEOUT_ENV, raising=False)
    assert usb_timeout_ms() == USB_TIMEOUT_MS == 1000


@pytest.mark.parametrize("value,expected", [("10000", 10000), (" 250 ", 250), ("", 1000)])
def test_usb_timeout_follows_the_environment(monkeypatch, value, expected):
    monkeypatch.setenv(USB_TIMEOUT_ENV, value)
    assert usb_timeout_ms() == expected


@pytest.mark.parametrize("value", ["0", "-5", "1.5", "nope"])
def test_usb_timeout_rejects_invalid_environment(monkeypatch, value):
    monkeypatch.setenv(USB_TIMEOUT_ENV, value)
    with pytest.raises(DapError, match="expected positive milliseconds"):
        usb_timeout_ms()


class _Selecting:
    # A link whose multidrop DP answers only from the `answers_from`th
    # selection on, and stops answering after its first read on the ones
    # before.
    def __init__(self, answers_from):
        self.port = types.SimpleNamespace(targetsel=0x01002927, abort=0x1E, mem_ap=True)
        self.clock_hz = 1_000_000
        self.selections = 0
        self.answers_from = answers_from

    def _command(self, data):
        return bytes([data[0], dap.PORT_SWD])

    def _wake(self):
        self.selections += 1

    def _dp_read(self, register):
        if self.selections < self.answers_from and register != dap.DP_DPIDR:
            raise dap.DapError("DAP transfer stopped after 0 of 1, ack 7")
        return dap.POWER_UP_ACK if register == dap.DP_CTRL_STAT else 0x0BC12477

    def _ap_read(self, register):
        return 0

    def _ap_write(self, register, value):
        pass

    def _dp_write(self, register, value):
        pass

    def _sequence(self, *args):
        pass


def test_a_multidrop_dp_that_resets_part_way_through_the_connect_is_connected_again(monkeypatch):
    monkeypatch.setattr(dap.time, "sleep", lambda s: None)
    link = _Selecting(answers_from=2)
    link._connect = lambda: dap._Link._connect(link)
    dap._Link._connect_settled(link)
    assert link.selections == 2


def test_a_multidrop_dp_that_never_answers_fails_the_connect(monkeypatch):
    monkeypatch.setattr(dap.time, "sleep", lambda s: None)
    link = _Selecting(answers_from=dap.MULTIDROP_ATTEMPTS + 1)
    link._connect = lambda: dap._Link._connect(link)
    with pytest.raises(dap.DapError, match="ack 7"):
        dap._Link._connect_settled(link)
    assert link.selections == dap.MULTIDROP_ATTEMPTS
