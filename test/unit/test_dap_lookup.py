# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness is found from the strings the kernel keeps in sysfs, not by
# asking a device that may be too busy to answer.

import usb.core

from test.rig.platforms.hardware import dap


class Interface:
    def __init__(self, number, string_index):
        self.bInterfaceNumber = number
        self.iInterface = string_index


class Configuration(list):
    bConfigurationValue = 1


class BusyDevice:
    # Every string request times out, as from a harness that is busy.
    def __init__(self, bus, ports, interfaces=()):
        self.bus = bus
        self.port_numbers = ports
        self.configuration = Configuration(interfaces)

    @property
    def serial_number(self):
        raise usb.core.USBTimeoutError("timed out", 110, 110)

    def get_active_configuration(self):
        return self.configuration


def sysfs(root, name, serial, interfaces=()):
    device = root / name
    device.mkdir(parents=True)
    (device / "serial").write_text(serial + "\n")
    for function, string in interfaces:
        (root / f"{name}:{function}").mkdir()
        (root / f"{name}:{function}" / "interface").write_text(string + "\n")


def test_a_busy_harness_is_found_by_its_sysfs_serial(monkeypatch, tmp_path):
    other, ours = BusyDevice(3, (7, 2)), BusyDevice(3, (7, 3))
    sysfs(tmp_path, "3-7.2", "D4D2E0179F4FECF3")
    sysfs(tmp_path, "3-7.3", "E6612483CB313225")
    monkeypatch.setattr(dap, "SYSFS_USB", tmp_path)
    monkeypatch.setattr(dap.usb.core, "find", lambda **kwargs: iter([other, ours]))
    assert dap.find_harness("E6612483CB313225") is ours
    assert dap.find_harness("0123456789ABCDEF") is None


def test_a_device_sysfs_cannot_name_falls_back_to_asking_it(monkeypatch, tmp_path):
    class Answering(BusyDevice):
        serial_number = "0123456789ABCDEF"

    device = Answering(9, None)
    monkeypatch.setattr(dap, "SYSFS_USB", tmp_path)
    monkeypatch.setattr(dap.usb.core, "find", lambda **kwargs: iter([device]))
    assert dap.find_harness("0123456789ABCDEF") is device


def test_interface_names_come_from_sysfs(monkeypatch, tmp_path):
    repl, dap_interface = Interface(0, 4), Interface(3, 7)
    device = BusyDevice(3, (7, 3), [repl, Interface(1, 0), dap_interface])
    sysfs(tmp_path, "3-7.3", "E6612483CB313225", [("1.0", "Metalclaw REPL"), ("1.3", dap.INTERFACE_NAME)])
    monkeypatch.setattr(dap, "SYSFS_USB", tmp_path)
    monkeypatch.setattr(dap.usb.util, "get_string", lambda *a: (_ for _ in ()).throw(AssertionError("asked")))
    assert dap.interface_strings(device) == [(repl, "Metalclaw REPL"), (dap_interface, dap.INTERFACE_NAME)]
