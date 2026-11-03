# SPDX-License-Identifier: Apache-2.0 OR MIT

import pytest

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
