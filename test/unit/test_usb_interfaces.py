# SPDX-License-Identifier: Apache-2.0 OR MIT

import errno

import pytest
import usb.core

from test.rig.platforms.hardware import platform


class Interface:
    iInterface = 4


class LeavingDevice:
    # A device whose configuration read fails with the given errno.
    def __init__(self, code):
        self.code = code

    def get_active_configuration(self):
        raise usb.core.USBError("gone", errno=self.code)


@pytest.fixture
def found(monkeypatch):
    def find(device):
        monkeypatch.setattr(platform.dap, "find_harness", lambda serial: device)
        monkeypatch.setattr(platform.usb.util, "dispose_resources", lambda device: None)
    return find


def test_a_device_that_leaves_mid_read_has_no_interfaces_yet(found):
    found(LeavingDevice(errno.ENODEV))
    assert platform.Harness("0123456789ABCDEF").usb_interfaces() == []


def test_any_other_usb_error_still_raises(found):
    found(LeavingDevice(errno.EIO))
    with pytest.raises(usb.core.USBError):
        platform.Harness("0123456789ABCDEF").usb_interfaces()


def test_interfaces_are_read_by_their_strings(found, monkeypatch):
    class Device:
        def get_active_configuration(self):
            return [Interface()]
    found(Device())
    monkeypatch.setattr(platform.usb.util, "get_string", lambda device, index: f"if{index}")
    assert platform.Harness("0123456789ABCDEF").usb_interfaces() == ["if4"]
