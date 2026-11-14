# SPDX-License-Identifier: Apache-2.0 OR MIT

import io
import itertools
import time

from test.rig.platforms.emulator import bench


OURS = "usbip://127.0.0.1:3240/1-0"


class FakeKernel:
    def __init__(self):
        self.ports = []
        self.serials = {}
        self.enumerated = False
        # The USB/IP targets whose export is gone.
        self.down = set()
        self.calls = []

    def usbip(self, cable, *args):
        self.calls.append(args)
        if args[0] == "attach":
            if cable.target in self.down:
                return 1, "usbip: error: import device"
            # As usbip does, the lowest free port.
            self.ports.append(next(p for p in itertools.count() if p not in self.ports))
        elif args[0] == "detach":
            self.ports.remove(int(args[2]))
        return 0, ""


def run_cable(monkeypatch, kernel, seconds, tmp_lock, meanwhile=None):
    monkeypatch.setattr(bench, "_imported_ports", lambda: list(kernel.ports))
    monkeypatch.setattr(bench, "_port_serial", lambda port: kernel.serials.get(port))
    monkeypatch.setattr(bench, "_enumerated", lambda serial: kernel.enumerated)
    monkeypatch.setattr(bench.Cable, "_usbip", lambda cable, *args: kernel.usbip(cable, *args))
    monkeypatch.setattr(bench, "REPLUG_AFTER_S", 0.3)
    monkeypatch.setattr(bench, "ATTACH_LOCK", tmp_lock)
    cable = bench.Cable("1-0", 3240, "0123456789ABCDEF", io.StringIO())
    cable.start()
    if meanwhile is not None:
        meanwhile(kernel)
    time.sleep(seconds)
    cable.stopping.set()
    cable.join(timeout=5)
    return cable


def test_an_import_that_never_enumerates_is_replugged(monkeypatch, tmp_path):
    kernel = FakeKernel()
    cable = run_cable(monkeypatch, kernel, 1.2, tmp_path / "lock")
    attaches = [c for c in kernel.calls if c[0] == "attach"]
    detaches = [c for c in kernel.calls if c[0] == "detach"]
    assert len(attaches) >= 2 and detaches
    assert detaches[0] == ("detach", "-p", "0")
    assert "replugging" in cable.log.getvalue()


def test_an_enumerated_import_is_left_alone(monkeypatch, tmp_path):
    kernel = FakeKernel()
    kernel.enumerated = True
    cable = run_cable(monkeypatch, kernel, 1.0, tmp_path / "lock")
    assert [c[0] for c in kernel.calls] == ["attach"]
    assert cable.imported.is_set()


def test_a_cable_replugs_and_releases_only_its_own_import(monkeypatch, tmp_path):
    kernel = FakeKernel()
    kernel.ports = [0]  # another bench's import
    cable = run_cable(monkeypatch, kernel, 1.2, tmp_path / "lock")
    cable.stop()
    detaches = [c for c in kernel.calls if c[0] == "detach"]
    assert detaches and all(c != ("detach", "-p", "0") for c in detaches)
    assert kernel.ports == [0]


def test_a_port_the_kernel_dropped_is_not_ours_once_another_bench_takes_it(monkeypatch, tmp_path):
    def meanwhile(kernel):
        time.sleep(0.3)
        # Our harness re-enumerates: the kernel drops the import and the
        # export is gone for a while.
        kernel.down.add(OURS)
        kernel.enumerated = False
        kernel.ports.remove(0)
        time.sleep(0.3)
        # Another bench attaches meanwhile, into the lowest free port.
        kernel.ports.append(0)

    kernel = FakeKernel()
    kernel.enumerated = True
    run_cable(monkeypatch, kernel, 1.0, tmp_path / "lock", meanwhile)
    assert ("detach", "-p", "0") not in kernel.calls
    assert kernel.calls[-1][0] == "attach"


def test_another_bench_attaching_on_our_port_within_one_poll_is_not_ours(monkeypatch, tmp_path):
    def meanwhile(kernel):
        time.sleep(0.3)
        other = bench.Cable("1-0", 3241, "00000000C0FFEE02", io.StringIO())
        # Between two of our polls the kernel drops our import while our
        # export is gone, and the other bench's attach takes the freed port.
        with bench._port_owners() as owners:
            kernel.down.add(OURS)
            kernel.enumerated = False
            kernel.ports.remove(0)
            other._poll(owners)
        assert kernel.ports == [0]

    kernel = FakeKernel()
    kernel.enumerated = True
    run_cable(monkeypatch, kernel, 1.0, tmp_path / "lock", meanwhile)
    assert ("detach", "-p", "0") not in kernel.calls
    assert kernel.calls[-1][0] == "attach"


def test_a_port_holding_another_serial_is_not_ours(monkeypatch, tmp_path):
    def meanwhile(kernel):
        time.sleep(0.3)
        # An import made outside the rig lands on our freed port number
        # between two polls, and enumerates.
        kernel.down.add(OURS)
        kernel.enumerated = False
        kernel.serials[0] = "0001"

    kernel = FakeKernel()
    kernel.enumerated = True
    run_cable(monkeypatch, kernel, 1.0, tmp_path / "lock", meanwhile)
    assert ("detach", "-p", "0") not in kernel.calls
    assert kernel.calls[-1][0] == "attach"
