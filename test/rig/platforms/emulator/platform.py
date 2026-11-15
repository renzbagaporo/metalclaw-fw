# SPDX-License-Identifier: Apache-2.0 OR MIT
# The emulator platform: the bench in Renode. The harness's USB reaches this
# host over USB/IP, so the harness, probe and DUT are driven exactly as on the
# hardware platform; only bringing the bench up differs.

import math
import os
from pathlib import Path

import pytest

from test.pico_examples.emulation import DEFAULT, Emulation, emulation_for
from test.pico_examples.parts import parts_for
from test.rig import probe_telemetry
from test.rig.platforms.emulator.bench import LAYOUTS, Bench, BenchError, Layout, harness_serial, instance
from test.rig.platforms.hardware.platform import (
    _DEFAULT_MAILBOX_TIMEOUT,
    ROOT,
    Dut,
    Example,
    Harness,
    _mailbox_timeout,
    _required,
    board_fixture,
    detach_all_but_probe,
    dut_image,
    example_metadata,
    hold_idle,
    require_example_image,
    require_rig_image,
    soc_series,
)

# The harness's MicroPython board picks the bench, as METALCLAW_BOARD_DIR
# picks the board `just build` builds.
HARNESS_BOARD_ENV = "METALCLAW_HARNESS_BOARD"
HARNESS_BOARD = "rpi_pi_pico"

# Host-side waits stretched for emulated time. Emulated SWD runs about 30x
# slower than the bench in host time; the target's own timing is unchanged.
SLOW_HOST_DEFAULTS = {
    "METALCLAW_PROBE_DOWNLOAD_TIMEOUT_S": "1800",
    "METALCLAW_PROBE_RESET_TIMEOUT_S": "600",
    "METALCLAW_PROBE_ACCESS_TIMEOUT_S": "600",
    # A single transfer block can stall past 10 s while the emulated harness
    # runs a flash routine on a loaded host.
    "CMSIS_DAP_USB_TIMEOUT_MS": "60000",
    # The last page's routine has no next page to overlap with, so its
    # whole run lands on one watchdog: 10-14 s of host time, past 30 s on a
    # loaded host. A timed-out routine is abandoned while it runs, and the
    # DUT locks up when it returns to a breakpoint with debug off.
    "FLASH_ALGORITHM_TIMEOUT_MULTIPLIER": "150",
    "ARM_DEBUG_TIMEOUT_MULTIPLIER": "30",
}
# Room for the cable to replug a few times (bench.REPLUG_AFTER_S).
ENUMERATION_TIMEOUT_S = 600.0


# Attaching a device that adds a USB function re-enumerates the harness, so
# its serial ports reappear on the same schedule as the harness itself.
class EmulatedHarness(Harness):
    def wait_enumerated(self, timeout: float = ENUMERATION_TIMEOUT_S) -> None:
        super().wait_enumerated(timeout)

    def serial_port(self, interface: str, timeout: float = ENUMERATION_TIMEOUT_S) -> str:
        return super().serial_port(interface, timeout)

    def wait_interfaces(self, settled, what: str, timeout: float = ENUMERATION_TIMEOUT_S) -> None:
        super().wait_interfaces(settled, what, timeout)

    def wait_interface(self, name: str, present: bool = True, timeout: float = ENUMERATION_TIMEOUT_S) -> None:
        super().wait_interface(name, present, timeout)

    # REPL deadlines are in bench seconds too, and the harness runs in the
    # same emulated time as the DUT.
    def run(self, code: str, timeout: float = 5.0) -> str:
        return super().run(code, timeout * _time_scale())


# Cases give DUT deadlines in bench seconds. The emulated DUT does the same
# work in the same emulated time, which passes about 80 times slower than
# host time even while the DUT idles; a boot that waits out an absent part
# takes ten minutes.
TIME_SCALE_ENV = "METALCLAW_EMULATOR_TIME_SCALE"
TIME_SCALE = 150.0


def _time_scale() -> float:
    value = os.environ.get(TIME_SCALE_ENV, TIME_SCALE)
    try:
        scale = float(value)
    except (TypeError, ValueError, OverflowError):
        scale = math.nan
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError(f"invalid {TIME_SCALE_ENV} {value!r}; expected a positive finite factor")
    return scale


class EmulatedDut(Dut):
    def __init__(self, harness: Harness, image: Path, toolchain_dir: str, scale: float, recorder=None):
        super().__init__(harness, image, toolchain_dir, recorder)
        self.scale = scale

    def ping(self, timeout=_DEFAULT_MAILBOX_TIMEOUT) -> dict:
        return super().ping(_mailbox_timeout(timeout) * self.scale)

    def command(self, line: str, timeout=_DEFAULT_MAILBOX_TIMEOUT) -> dict:
        return super().command(line, _mailbox_timeout(timeout) * self.scale)

    def start(self, line: str, timeout: float = 10.0) -> int:
        return super().start(line, timeout * self.scale)

    def finish(self, sequence: int, timeout: float = 10.0) -> dict:
        return super().finish(sequence, timeout * self.scale)


def harness_board() -> str:
    return os.environ.get(HARNESS_BOARD_ENV, HARNESS_BOARD)


def layout() -> Layout:
    board = harness_board()
    if board not in LAYOUTS:
        pytest.exit(f"no emulated bench for harness board {board!r}; {HARNESS_BOARD_ENV} is one of "
                    f"{', '.join(sorted(LAYOUTS))}", returncode=2)
    return LAYOUTS[board]


# The bench script fixes the emulated harness's serial, per instance;
# METALCLAW_HARNESS_SERIAL names a bench harness and is left to the hardware
# platform.
def _instance() -> int:
    try:
        return instance()
    except BenchError as error:
        pytest.exit(str(error), returncode=2)


def _serial() -> str:
    try:
        return harness_serial(layout(), _instance())
    except BenchError as error:
        pytest.exit(str(error), returncode=2)


@pytest.fixture(scope="session")
def fixture(harness: Harness) -> dict:
    return board_fixture(harness)


# What the session's DUT image needs of the bench, or the defaults where no
# image is there yet (the bench fixture then stops the run).
def image_emulation() -> Emulation:
    try:
        image = dut_image()
    except pytest.exit.Exception:
        return DEFAULT
    return emulation_for(example_metadata(image))


# A session whose program runs with stand-ins inside the emulated DUT
# reports every result under a name that says so, and as a property, so no
# such pass is ever taken for one through the harness.
def pytest_generate_tests(metafunc):
    if "emulation" not in metafunc.fixturenames:
        return
    emulation = image_emulation()
    if emulation.stand_ins:
        metafunc.parametrize("emulation", [emulation], ids=[emulation.stand_in_label], indirect=True,
                             scope="session")


def pytest_collection_modifyitems(items):
    for item in items:
        emulation = getattr(getattr(item, "callspec", None), "params", {}).get("emulation")
        if emulation is not None and emulation.stand_ins:
            item.user_properties.append(("emulator_stand_ins", " ".join(s.label for s in emulation.stand_ins)))


def session_emulation(param: Emulation | None) -> Emulation:
    return param or image_emulation()


@pytest.fixture(scope="session")
def emulation(request) -> Emulation:
    return session_emulation(getattr(request, "param", None))


# What the session saw of the emulation itself, read at its end: in the
# junit file's properties and the terminal summary.
DIAGNOSTICS = pytest.StashKey[list[tuple[str, str]]]()
# The session's harness, the `console` fixtures set up and not yet read,
# and the junit recorder of the testsuite's properties.
_session: dict = {"harness": None, "consoles": [], "record": None}


def record_diagnostic(config: pytest.Config, name: str, value) -> None:
    config.stash.setdefault(DIAGNOSTICS, []).append((name, str(value)))
    if _session["record"] is not None:
        _session["record"](name, str(value))


def record_late_deliveries(config: pytest.Config, counts: dict[str, int | None]) -> None:
    for wire, count in counts.items():
        record_diagnostic(config, f"renode_{wire}_late_deliveries",
                          "absent from this Renode package" if count is None else count)


def pytest_terminal_summary(terminalreporter, exitstatus, config):
    found = config.stash.get(DIAGNOSTICS, [])
    if found:
        terminalreporter.write_sep("-", "emulator diagnostics")
        for name, value in found:
            terminalreporter.write_line(f"{name}: {value}")


# Every console bridge on the harness, one line each: its name, then as
# key=value its UART's frame errors and overruns, those split into the
# receiver's FIFO and the port's ring, and the host writes it took short.
# A harness built before a counter existed leaves that one out.
CONSOLE_COUNTERS = ("frame_errors", "overruns", "fifo_overruns", "ring_overruns")
CONSOLE_STATS = f"""import metalclaw
for i in metalclaw.instances():
    if i.model == 'console_bridge':
        s = i.stats('uart')
        c = [(k, s[k]) for k in {CONSOLE_COUNTERS!r} if k in s]
        try:
            c.append(('short_writes', i.short_writes))
        except AttributeError:
            pass
        print(i.name, ' '.join('%s=%d' % kv for kv in c))
"""
# Bench seconds: an emulated REPL round trip, with room for a loaded host.
CONSOLE_STATS_TIMEOUT_S = 2.0


def record_console_stats(config: pytest.Config, harness: Harness) -> None:
    try:
        text = harness.run(CONSOLE_STATS, CONSOLE_STATS_TIMEOUT_S)
    except Exception as error:
        record_diagnostic(config, "console_bridge_stats", f"unavailable: {error}")
        return
    for line in text.splitlines():
        name, *counters = line.split()
        for counter in counters:
            key, _, value = counter.partition("=")
            if key and value.isdigit():
                record_diagnostic(config, f"console_bridge_{name}_{key}", value)


# A program's console bridge lives as long as the module's `console`
# fixture, so its counters are read at that module's last teardown, before
# the fixture detaches it.
@pytest.hookimpl(wrapper=True)
def pytest_fixture_setup(fixturedef, request):
    result = yield
    if fixturedef.argname == "console":
        _session["consoles"].append(fixturedef)
    return result


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_teardown(item, nextitem):
    if nextitem is not None and nextitem.path == item.path:
        return
    consoles = [f for f in _session["consoles"] if f.cached_result is not None]
    if consoles and _session["harness"] is not None:
        _session["consoles"] = [f for f in _session["consoles"] if f not in consoles]
        record_console_stats(item.config, _session["harness"])


@pytest.fixture(scope="session")
def bench(emulation: Emulation, pytestconfig, record_testsuite_property):
    _session["record"] = record_testsuite_property
    for name, value in SLOW_HOST_DEFAULTS.items():
        os.environ.setdefault(name, value)
    bench_layout = layout()
    image = dut_image()
    series = soc_series(image)
    if series != bench_layout.dut_chip:
        pytest.exit(f"the {harness_board()} bench's DUT is an {bench_layout.dut_chip}, but {image} is built for "
                    f"{series!r}; build it with the matching METALCLAW_DUT_BOARD", returncode=2)
    b = Bench(
        renode_dir=Path(_required("METALCLAW_RENODE_DIR")),
        layout=bench_layout,
        harness_uf2=Path(os.environ.get("METALCLAW_HARNESS_IMAGE",
                                        ROOT / f"build/rp2/{harness_board()}/firmware.uf2")),
        roms=Path(_required("METALCLAW_RENODE_ROMS")),
        dut_image=image,
        work=Path(os.environ.get("METALCLAW_RENODE_WORK", ROOT / "build/emulator")),
        serial=_serial(),
        instance=_instance(),
        emulation=emulation,
    )
    # Bench.start and Bench.stop between them stop Renode on every way out,
    # an interrupt or pytest.exit in the middle of either included.
    try:
        try:
            b.start()
        except BenchError as error:
            pytest.exit(str(error), returncode=2)
        yield b
        record_late_deliveries(pytestconfig, b.late_deliveries())
    finally:
        b.stop()


@pytest.fixture(scope="session")
def harness(bench, pytestconfig) -> Harness:
    recorder = probe_telemetry.recorder(pytestconfig)
    if recorder is not None:
        recorder.describe(bench={"serial": _serial(), "board": harness_board(), "instance": _instance()})
    h = EmulatedHarness(_serial())
    h.wait_enumerated()
    detach_all_but_probe(h, recorder)
    _session["harness"] = h
    return h


@pytest.fixture(scope="session")
def probe(harness: Harness, fixture: dict, pytestconfig):
    if "CMSIS-DAP v2" not in harness.usb_interfaces():
        harness.run(f"import metalclaw\nmetalclaw.attach('swd_probe', 'dut', swd={fixture['swd']})")
        try:
            harness.wait_interface("CMSIS-DAP v2")
        except TimeoutError as error:
            pytest.exit(f"{error} after attaching swd_probe", returncode=2)
    yield harness.probe_id
    probe_telemetry.read_end(pytestconfig, harness)


# The bench started with the image already in the DUT's flash; swd_probe's
# cases download it through the probe. The DUT booted with the bench, before
# the harness could hold any idle level, so it boots again once they are. A
# program that needs what this Renode package's bench script does not take
# is not judged on it; a part a stand-in replaces is not attached.
@pytest.fixture(scope="session")
def example(bench: Bench, harness: Harness, probe, fixture: dict, emulation: Emulation, pytestconfig) -> Example:
    require_example_image(bench.dut_image)
    e = Example(harness, bench.dut_image, _required("METALCLAW_ARM_TOOLCHAIN_DIR"),
                probe_telemetry.recorder(pytestconfig))
    if bench.settings.unmet:
        pytest.skip(f"{e.target} needs {', '.join(bench.settings.unmet)}, which {bench.layout.script} in this "
                    f"Renode package does not take")
    e.replaced_parts = frozenset(s.replaces for s in emulation.stand_ins)
    clashes = stand_in_clashes(e.target, e.metadata.get("board"), emulation)
    if clashes:
        pytest.exit(f"{e.target}'s stand-ins share pins with harness parts: {', '.join(clashes)}", returncode=2)
    hold_idle(harness, fixture, e.parts_dut_pins() | {p for s in emulation.stand_ins for p in s.pins})
    e.restart()
    return e


# The harness parts still attached that take a stand-in's pins: the
# harness would then drive a pin the stand-in drives inside the DUT.
def stand_in_clashes(target: str, board: str | None, emulation: Emulation) -> list[str]:
    taken = {p for s in emulation.stand_ins for p in s.pins}
    attached = parts_for(target, board, {s.replaces for s in emulation.stand_ins})
    return [f"{part.name} on GP{sorted(taken & set(part.dut_pins()))}" for part in attached
            if taken & set(part.dut_pins())]


@pytest.fixture(scope="session")
def dut(bench: Bench, harness: Harness, probe, fixture: dict, pytestconfig) -> Dut:
    require_rig_image(bench.dut_image)
    d = EmulatedDut(harness, bench.dut_image, _required("METALCLAW_ARM_TOOLCHAIN_DIR"), _time_scale(),
                    probe_telemetry.recorder(pytestconfig))
    hold_idle(harness, fixture)
    d.reset()
    d.ping()
    return d
