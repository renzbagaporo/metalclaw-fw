# SPDX-License-Identifier: Apache-2.0 OR MIT

import io
import time

from test.rig.platforms.emulator import bench


class FakeKernel:
    def __init__(self):
        self.ports = []
        self.enumerated = False
        self.calls = []

    def usbip(self, *args):
        self.calls.append(args)
        if args[0] == "attach":
            self.ports = [0]
        elif args[0] == "detach":
            self.ports = []
        return 0, ""


def run_cable(monkeypatch, kernel, seconds):
    monkeypatch.setattr(bench, "_imported_ports", lambda: list(kernel.ports))
    monkeypatch.setattr(bench, "_enumerated", lambda serial: kernel.enumerated)
    monkeypatch.setattr(bench.Cable, "_usbip", lambda cable, *args: kernel.usbip(*args))
    monkeypatch.setattr(bench, "REPLUG_AFTER_S", 0.3)
    cable = bench.Cable("1-0", 3240, "0123456789ABCDEF", io.StringIO())
    cable.start()
    time.sleep(seconds)
    cable.stopping.set()
    cable.join(timeout=5)
    return cable


def test_an_import_that_never_enumerates_is_replugged(monkeypatch):
    kernel = FakeKernel()
    cable = run_cable(monkeypatch, kernel, 1.2)
    attaches = [c for c in kernel.calls if c[0] == "attach"]
    detaches = [c for c in kernel.calls if c[0] == "detach"]
    assert len(attaches) >= 2 and detaches
    assert detaches[0] == ("detach", "-p", "0")
    assert "replugging" in cable.log.getvalue()


def test_an_enumerated_import_is_left_alone(monkeypatch):
    kernel = FakeKernel()
    kernel.enumerated = True
    cable = run_cable(monkeypatch, kernel, 1.0)
    assert [c[0] for c in kernel.calls] == ["attach"]
    assert cable.imported.is_set()
