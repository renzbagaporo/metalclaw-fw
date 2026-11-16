# SPDX-License-Identifier: Apache-2.0 OR MIT
# The DUT's own USB in the emulator: the bench exports it as bus 1-1 beside
# the harness's 1-0, the rig imports it for the cases that ask, finds the
# program's device by its VID:PID on its own vhci port, and judges what it
# presents and prints there.

import io
import re
import time
import types

import pytest

from test.pico_examples import cases
from test.pico_examples.cases import Typist, read_console, read_reply
from test.pico_examples.targets import SPECS, Spec
from test.pico_examples.usb_devices import USB_DEVICES, UsbDevice, usb_device
from test.rig.platforms.emulator import bench, platform
from test.rig.platforms.emulator.bench import UsbFunctions


def vhci(tmp_path, rows):
    status = tmp_path / "status"
    status.write_text("hub port sta spd dev      sockfd local_busid\n" + "".join(
        f"hs  {port:04d} {sta:03d} 002 00010000 000003 {busid}\n" for port, sta, busid in rows))
    return status


def device_in_sysfs(root, name, vid, pid, interfaces=()):
    device = root / name
    device.mkdir(parents=True)
    (device / "idVendor").write_text(f"{vid:04x}\n")
    (device / "idProduct").write_text(f"{pid:04x}\n")
    for number, label, kind, node in interfaces:
        interface = root / f"{name}:1.{number}"
        interface.mkdir()
        (interface / "bInterfaceNumber").write_text(f"{number:02x}\n")
        if label is not None:
            (interface / "interface").write_text(label + "\n")
        if kind == "tty":
            (interface / "tty" / node).mkdir(parents=True)
        elif kind == "hid":
            (interface / f"0003:{vid:04X}:{pid:04X}.0001" / "hidraw" / node).mkdir(parents=True)
        elif kind == "ep":
            for ep in node:
                (interface / f"ep_{ep}").mkdir()
    return device


@pytest.fixture
def sysfs(tmp_path, monkeypatch):
    usb = tmp_path / "usb"
    usb.mkdir()
    monkeypatch.setattr(bench, "USB_DEVICES", usb)
    return usb


def test_the_bench_exports_the_duts_usb_beside_the_harnesses():
    assert (bench.USBIP_BUSID, bench.DUT_USBIP_BUSID) == ("1-0", "1-1")


def test_a_devices_functions_come_in_interface_order_with_their_strings(sysfs):
    # dev_multi_cdc: two CDC functions (comm interfaces 0 and 2) and a
    # vendor reset interface. The sysfs order is not the interface order.
    device_in_sysfs(sysfs, "9-1", 0xCAFE, 0x4102, [
        (2, "Custom CDC", "tty", "ttyACM7"),
        (0, "Pico SDK stdio", "tty", "ttyACM6"),
        (4, "Reset", None, None),
    ])
    found = bench.usb_functions("9-1")
    assert found == UsbFunctions("9-1", 0xCAFE, 0x4102,
                                 (("Pico SDK stdio", "/dev/ttyACM6"), ("Custom CDC", "/dev/ttyACM7")))


def test_a_hid_function_is_its_hidraw_node_and_a_vendor_one_its_endpoints(sysfs):
    device_in_sysfs(sysfs, "9-2", 0xCAFE, 0x4004, [(0, None, "hid", "hidraw3")])
    assert bench.usb_functions("9-2").hidraw == ("/dev/hidraw3",)
    device_in_sysfs(sysfs, "9-3", 0x0000, 0x0001, [(0, None, "ep", ("82", "01"))])
    found = bench.usb_functions("9-3")
    assert (found.vid, found.pid, found.endpoints, found.ttys) == (0, 1, ("01", "82"), ())


def test_a_device_gone_from_sysfs_has_no_functions(sysfs):
    assert bench.usb_functions("9-9") is None


def test_a_ports_identity_is_its_enumerated_devices_vid_pid(tmp_path, sysfs, monkeypatch):
    monkeypatch.setattr(bench, "VHCI_STATUS", vhci(tmp_path, [(0, 6, "9-1"), (1, 5, "0-0")]))
    device_in_sysfs(sysfs, "9-1", 0x2E8A, 0x000A)
    assert bench._port_identity(0) == (0x2E8A, 0x000A)
    assert bench._port_identity(1) is None
    assert bench._port_device(0) == "9-1"


def dut_cable(monkeypatch, tmp_path, identities, ports):
    monkeypatch.setattr(bench, "_port_identity", lambda port: identities.get(port))
    monkeypatch.setattr(bench, "_imported_ports", lambda: list(ports))
    monkeypatch.setattr(bench, "ATTACH_LOCK", tmp_path / "lock")
    return bench.DutCable(bench.DUT_USBIP_BUSID, 3246, 0x2E8A, 0x000A, io.StringIO())


def test_a_dut_cable_carries_only_its_programs_device(monkeypatch, tmp_path):
    cable = dut_cable(monkeypatch, tmp_path, {0: (0x2E8A, 0x000A), 1: (0x2FE3, 0x0204)}, [0, 1, 2])
    assert cable.target == "usbip://127.0.0.1:3246/1-1"
    assert cable._carries(0) and cable._carries(2)
    assert not cable._carries(1)
    assert cable._enumerated([0]) and not cable._enumerated([1, 2])


def test_a_dut_cable_finds_the_device_on_its_own_port_only(monkeypatch, tmp_path):
    # Another instance runs the same program: the same VID:PID on port 0.
    cable = dut_cable(monkeypatch, tmp_path, {0: (0x2E8A, 0x000A), 3: (0x2E8A, 0x000A)}, [0, 3])
    monkeypatch.setattr(bench, "_port_device", lambda port: f"9-{port + 1}")
    seen = []
    monkeypatch.setattr(bench, "usb_functions", lambda device: seen.append(device) or device)
    assert cable.functions() is None
    with bench._port_owners() as owners:
        owners[3] = cable.target
        owners[0] = "usbip://127.0.0.1:3245/1-1"
    assert cable.functions() == "9-4"
    assert seen == ["9-4"]


def test_a_dut_import_that_never_enumerates_is_replugged(monkeypatch, tmp_path):
    ports, calls = [], []

    def usbip(cable, *args):
        calls.append(args)
        if args[0] == "attach":
            ports.append(len(ports))
        else:
            ports.remove(int(args[2]))
        return 0, ""

    cable = dut_cable(monkeypatch, tmp_path, {}, ports)
    monkeypatch.setattr(bench, "_imported_ports", lambda: list(ports))
    monkeypatch.setattr(bench.Cable, "_usbip", usbip)
    monkeypatch.setattr(bench, "REPLUG_AFTER_S", 0.3)
    cable.start()
    time.sleep(1.2)
    cable.stop()
    assert [c[0] for c in calls].count("attach") >= 2
    assert ("attach", "-r", "127.0.0.1", "-b", "1-1") in calls
    assert ports == []


# ---- the bench -------------------------------------------------------------


class FakeCable:
    def __init__(self, *args):
        self.args = args
        self.started = self.stopped = False

    def start(self):
        self.started = True

    def stop(self):
        self.stopped = True


def test_the_duts_usb_is_imported_only_while_the_bench_runs(tmp_path, monkeypatch):
    monkeypatch.setattr(bench, "DutCable", FakeCable)
    b = bench.Bench(tmp_path, bench.LAYOUTS["metalclaw_pico_rp2"], tmp_path / "h.uf2", tmp_path, tmp_path / "d.elf",
                    tmp_path, "00000000C0FFEE07", 6)
    with pytest.raises(bench.BenchError, match="not running"):
        b.import_dut_usb(0x2E8A, 0x000A)
    b.process, b.usbip_port, b.dut_usbip_port = types.SimpleNamespace(), 3246, 3256
    cable = b.import_dut_usb(0x2E8A, 0x000A)
    # On the DUT's own server, not the harness's.
    assert cable.started and cable.args[:4] == ("1-1", 3256, 0x2E8A, 0x000A)
    with pytest.raises(bench.BenchError, match="already imported"):
        b.import_dut_usb(0x2E8A, 0x000A)
    b.release_dut_usb()
    assert cable.stopped and b.dut_cable is None


# ---- the fixture -----------------------------------------------------------


class SequenceCable:
    def __init__(self, *found):
        self.found = list(found)

    def functions(self):
        return self.found.pop(0) if len(self.found) > 1 else self.found[0]


HELLO = UsbDevice(0x2E8A, 0x000A, ("Board CDC",))


def test_the_fixture_waits_for_the_devices_functions_to_bind():
    bare = UsbFunctions("9-1", 0x2E8A, 0x000A)
    bound = UsbFunctions("9-1", 0x2E8A, 0x000A, (("Board CDC", "/dev/ttyACM0"),))
    cable = SequenceCable(None, None, bare, bare, bound)
    assert platform.wait_usb_functions(cable, HELLO, 5, 5, 0.01) == bound


def test_a_device_that_binds_less_than_it_should_is_returned_once_its_time_is_up():
    bare = UsbFunctions("9-1", 0x2E8A, 0x000A)
    started = time.monotonic()
    assert platform.wait_usb_functions(SequenceCable(bare), HELLO, 60, 0.2, 0.01) == bare
    assert time.monotonic() - started < 5


def test_a_device_that_never_enumerates_is_none():
    assert platform.wait_usb_functions(SequenceCable(None), HELLO, 0.2, 60, 0.01) is None


def test_bound_needs_every_function_the_device_should_present():
    device = UsbDevice(0xCAFE, 0x4102, ("Pico SDK stdio", "Custom CDC"))
    one = UsbFunctions("9-1", 0xCAFE, 0x4102, (("Pico SDK stdio", "/dev/ttyACM0"),))
    two = UsbFunctions("9-1", 0xCAFE, 0x4102, one.ttys + (("Custom CDC", "/dev/ttyACM1"),))
    assert not platform.usb_bound(one, device) and platform.usb_bound(two, device)
    assert not platform.usb_bound(UsbFunctions("9-1", 0, 1), UsbDevice(0, 1, endpoints=(0x01, 0x82)))
    assert not platform.usb_bound(UsbFunctions("9-1", 0xCAFE, 0x4004), UsbDevice(0xCAFE, 0x4004, hidraw=1))


def test_the_hardware_platform_has_no_duts_usb():
    from test.rig.platforms.hardware import platform as hardware
    fixture = hardware.dut_usb
    function = getattr(fixture, "_get_wrapped_function", lambda: fixture.__wrapped__)()
    with pytest.raises(pytest.skip.Exception, match="reaches no rig host on either bench"):
        function()


# ---- the programs ----------------------------------------------------------


def test_the_stdio_usb_programs_present_the_sdks_cdc_with_each_chips_pid():
    for target in ("hello_usb", "hello_anything"):
        assert usb_device(target, "rp2040") == UsbDevice(0x2E8A, 0x000A, ("Board CDC",),
                                                         source=usb_device(target, "rp2040").source)
        assert usb_device(target, "rp2350").pid == 0x0009
    assert usb_device("hello_serial", "rp2040") is None


def test_every_usb_console_spec_reads_a_cdc_function_of_its_programs_device():
    judged = {t for t, s in SPECS.items() if s.usb_console}
    assert judged == {"hello_usb", "hello_anything", "dev_multi_cdc"}
    for target in judged:
        spec = SPECS[target]
        for chip in ("rp2040", "rp2350"):
            device = usb_device(target, chip)
            assert device is not None and device.ttys, (target, chip)
            assert all(name in device.ttys for _, name, _ in spec.usb_input), target
            assert all(name in device.ttys for name, _ in spec.usb_replies), target
        for pattern in spec.usb_console + tuple(p for p, _, _ in spec.usb_input) + tuple(p for _, p in spec.usb_replies):
            re.compile(pattern)


def test_a_program_printing_only_on_usb_is_not_judged_on_its_uart():
    assert SPECS["hello_usb"].console == () and SPECS["dev_multi_cdc"].console == ()
    assert SPECS["hello_anything"].console == SPECS["hello_anything"].usb_console


def test_every_usb_device_names_its_source():
    assert all(d.source for devices in USB_DEVICES.values() for d in devices.values())


class FakePort:
    def __init__(self, chunks=()):
        self.chunks = list(chunks)
        self.written = []

    def read(self, size):
        return self.chunks.pop(0) if self.chunks else b""

    def write(self, data):
        self.written.append(data)

    def flush(self):
        pass


def test_a_named_input_goes_to_that_port():
    console, custom = FakePort(), FakePort()
    t = Typist(console, ((r"Connected", ("Custom CDC", b"metalclaw")),), None, {"Custom CDC": custom})
    t("Connected to CDC 0\r\n")
    assert custom.written == [b"metalclaw"] and console.written == []


class Clock:
    def __init__(self):
        self.now = 0.0

    def clock(self):
        self.now += 1.0
        return self.now


def test_dev_multi_cdc_is_judged_by_the_answer_on_its_second_cdc():
    spec = SPECS["dev_multi_cdc"]

    class Custom(FakePort):
        # The program: what comes in on CDC 1 is answered OK there.
        def read(self, size):
            if self.written and not getattr(self, "answered", False):
                self.answered = True
                return b"OK\r\n"
            return super().read(size)

    custom = Custom()
    console = FakePort([b"Connected to CDC 0\r\n"])
    inputs = tuple((p, (name, data)) for p, name, data in spec.usb_input)
    text = read_console(Clock(), console, spec, spec.usb_console, inputs, {"Custom CDC": custom})
    assert spec.match(text, spec.usb_console) == ([], [])
    assert custom.written == [b"metalclaw"]
    ((name, pattern),) = spec.usb_replies
    assert name == "Custom CDC" and re.search(pattern, read_reply(Clock(), custom, pattern, spec))
    assert spec.match(text) == ([], [])  # no UART console criteria at all


def test_a_reply_read_gives_up_at_the_specs_deadline():
    spec = Spec(console=(), seconds=1000.0)
    assert read_reply(Clock(), FakePort([b"nope"]), r"OK", spec) == "nope"


def test_a_usb_console_read_ends_once_its_own_patterns_matched():
    spec = Spec(console=("never",), usb_console=("hello",), seconds=1000.0)
    port = FakePort([b"hello"])
    assert read_console(Clock(), port, spec, spec.usb_console) == "hello"


def test_the_usb_cases_come_after_every_case_that_restarts_the_program():
    # The import lives as long as the module's first case that asks for it:
    # dev_hid_composite's LED pin case expects an unmounted device.
    names = [n for n in vars(cases) if n.startswith("test_")]
    usb = [names.index("test_the_example_presents_its_usb_device"),
           names.index("test_the_example_completes_on_its_usb_console")]
    assert min(usb) > max(i for i, n in enumerate(names) if n not in (
        "test_the_example_presents_its_usb_device", "test_the_example_completes_on_its_usb_console"))


def test_an_import_the_server_never_answers_fails_the_cases_that_asked_for_it(monkeypatch):
    import threading

    class Unanswered:
        target = "usbip://127.0.0.1:3246/1-1"
        imported = threading.Event()

    released = []
    bench_ = types.SimpleNamespace(import_dut_usb=lambda vid, pid: Unanswered(), dut_usb_unmet=lambda: None,
                                   release_dut_usb=lambda: released.append(True), layout=bench.LAYOUTS["rpi_pi_pico"])
    example = types.SimpleNamespace(target="hello_usb", metadata={"chip": "rp2040"})
    monkeypatch.setattr(platform, "wait_presenting", lambda harness, example: True)
    monkeypatch.setattr(platform, "DUT_USB_IMPORT_S", 0.05)
    fixture = platform.dut_usb
    function = getattr(fixture, "_get_wrapped_function", lambda: fixture.__wrapped__)()
    with pytest.raises(pytest.fail.Exception, match="did not answer the import .*1-1"):
        next(function(bench_, None, example))
    assert released == [True]


def test_a_bench_script_without_the_duts_usbip_port_leaves_its_usb_unmet(tmp_path, monkeypatch):
    monkeypatch.setattr(bench, "DutCable", FakeCable)
    b = bench.Bench(tmp_path, bench.LAYOUTS["rpi_pi_pico"], tmp_path / "h.uf2", tmp_path, tmp_path / "d.elf",
                    tmp_path, "0123456789ABCDEF")
    assert b.dut_usb_unmet() is None
    b.process, b.usbip_port = types.SimpleNamespace(), 3240
    assert b.dut_usb_unmet() == ("$dut_usbip_port, which scripts/multi-node/metalclaw_fixture_one_machine.resc in "
                                 "this Renode package does not take")
    with pytest.raises(bench.BenchError, match=r"needs \$dut_usbip_port"):
        b.import_dut_usb(0x2E8A, 0x0009)


def test_the_fixture_skips_a_package_without_the_duts_server_at_once(monkeypatch):
    imported = []
    bench_ = types.SimpleNamespace(import_dut_usb=lambda vid, pid: imported.append(True),
                                   dut_usb_unmet=lambda: "$dut_usbip_port, which x.resc does not take")
    example = types.SimpleNamespace(target="hello_usb", metadata={"chip": "rp2040"})
    monkeypatch.setattr(platform, "wait_presenting", lambda harness, example: pytest.fail("waited"))
    fixture = platform.dut_usb
    function = getattr(fixture, "_get_wrapped_function", lambda: fixture.__wrapped__)()
    with pytest.raises(pytest.skip.Exception, match=r"hello_usb's own USB needs \$dut_usbip_port"):
        next(function(bench_, None, example))
    assert imported == []


def test_the_duts_usbip_port_is_set_only_where_the_script_takes_it():
    def run(port):
        return bench.monitor_commands(bench.Path("/r/bench.resc"), bench.Path("/w/rom"), bench.Path("/w/flash"),
                                      bench.Path("/r/dut"), bench.Path("/w/dut"), "pico", None, 3246, None, port)
    with_port = run(3256)
    assert with_port.index("$dut_usbip_port=3256") < with_port.index("include @/r/bench.resc")
    assert not any(c.startswith("$dut_usbip_port") for c in run(None))


def bench_package(root, with_dut_port):
    layout = bench.LAYOUTS["metalclaw_pico_rp2"]
    for name in ("renode", "roms/b2.elf", "roms/bootrom-combined.bin", "h.uf2", "d.elf"):
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_text("")
    script = root / layout.script
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text("$usbip_port?=3240\n" + ("$dut_usbip_port?=3250\n" if with_dut_port else ""))
    return bench.Bench(root, layout, root / "h.uf2", root / "roms", root / "d.elf", root / "work",
                       "00000000C0FFEE07", 6)


@pytest.mark.parametrize("with_dut_port", [True, False])
def test_each_instance_gives_the_dut_its_own_usbip_port(tmp_path, monkeypatch, with_dut_port):
    class Launched(Exception):
        pass

    def popen(argv, **kwargs):
        raise Launched([argv[i + 1] for i, arg in enumerate(argv) if arg == "-e"])

    checked = []
    monkeypatch.setattr(bench, "_listening", lambda port: checked.append(port) or False)
    monkeypatch.setattr(bench.dut_flash, "flash_contents", lambda elf, **kw: b"dut")
    monkeypatch.setattr(bench.uf2, "flash_contents", lambda data, chip: b"harness")
    monkeypatch.setattr(bench.storage, "with_storage", lambda image: image)
    monkeypatch.setattr(bench.subprocess, "Popen", popen)
    b = bench_package(tmp_path, with_dut_port)
    monkeypatch.setattr(b, "_rom", lambda chip, name: tmp_path / name)
    with pytest.raises(Launched) as launched:
        b.start()
    commands = launched.value.args[0]
    assert "$usbip_port=3246" in commands
    assert ("$dut_usbip_port=3256" in commands) == with_dut_port
    assert checked == ([1240, 3246, 3256] if with_dut_port else [1240, 3246])


def test_the_console_goes_to_its_log_file_as_it_arrives(tmp_path, monkeypatch):
    log = tmp_path / "console.log"
    monkeypatch.setenv("METALCLAW_CONSOLE_LOG", str(log))
    spec = Spec(console=("world",), seconds=1000.0)
    assert read_console(Clock(), FakePort([b"hello ", b"world"]), spec) == "hello world"
    assert log.read_bytes() == b"hello world"
