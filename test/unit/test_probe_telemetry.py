# SPDX-License-Identifier: Apache-2.0 OR MIT

import contextlib
import io
import json
import struct
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

from test.rig import probe_telemetry
from test.rig.platforms.hardware import dap, platform
from test.rig.platforms.hardware.dap import DapError, DapMemory
from test.rig.platforms.hardware.mailbox import Mailbox
from test.rig.platforms.hardware.probe import ProbeError, ProbeRs
from test.rig.probe_telemetry import Discard, Recorder, Timing

pytest_plugins = ["pytester"]


class Clock:
    def __init__(self):
        self.now = 100.0

    def __call__(self):
        return self.now


def recorder():
    return Recorder("rig", clock=Clock(), wall=lambda: 1_700_000_000.0)


def test_a_percentile_is_within_a_bucket_of_the_true_duration():
    timing = Timing()
    for _ in range(99):
        timing.add(0.001)
    timing.add(1.0)
    for q in (0.5, 0.95, 0.99):
        assert 0.001 <= timing.percentile(q) <= 0.001 * 2 ** (1 / 8)
    assert timing.percentile(1.0) == timing.max == 1.0
    assert timing.count == 100


def test_a_percentile_never_exceeds_the_largest_duration():
    timing = Timing()
    timing.add(0.0105)
    assert timing.percentile(0.5) == 0.0105


def test_an_empty_timing_has_no_percentile():
    assert Timing().percentile(0.5) is None


def test_timings_merge_and_survive_json():
    first, second = Timing(), Timing()
    first.add(0.002)
    second.add(0.5)
    second.add(0.0)
    first.merge(Timing.from_json(json.loads(json.dumps(second.to_json()))))
    assert first.count == 3
    assert first.max == 0.5
    assert first.total == pytest.approx(0.502)
    assert sum(first.buckets.values()) == 3


def test_calls_are_ok_plus_errors_and_a_rejection_moves_one_from_ok():
    r = recorder()
    r.record("dap", "read", 0.001, words=128)
    r.record("dap", "read", 0.002, words=128)
    r.record("dap", "read", 1.0, "usb_timeout", "timed out")
    r.reject("dap", "read", "mismatch", "word 3")
    operation = r.operations[("dap", "read")]
    assert (operation.calls, operation.ok, operation.errors) == (3, 1, {"usb_timeout": 1, "mismatch": 1})
    assert operation.words == 256
    assert [sample["kind"] for sample in r.samples] == ["usb_timeout", "mismatch"]


def test_retries_count_and_say_whether_they_recovered():
    r = recorder()
    r.retry("dap", "read", True)
    r.retry("dap", "read", False)
    operation = r.operations[("dap", "read")]
    assert (operation.retries, operation.recovered) == (2, 1)


def test_samples_are_bounded_and_truncated():
    r = recorder()
    for _ in range(probe_telemetry.SAMPLES + 5):
        r.record("probe_rs", "reset", 0.1, "exit", "x" * 1000)
    assert len(r.samples) == probe_telemetry.SAMPLES
    assert len(r.samples[0]["message"]) == probe_telemetry.MESSAGE_CHARS


def snapshot(transfers, errors=0, short_replies=0, board="rpi_pi_pico"):
    return {"board": board, "probes": [{"name": "dut", "enabled": True, "clock_hz": 1000000, "connected": False,
                                        "transfers": transfers, "errors": errors, "short_replies": short_replies}]}


def test_the_session_line_carries_bench_dut_operations_and_probe_counters():
    r = recorder()
    r.describe(platform="hardware", bench={"serial": "SER1"}, dut={"chip": "RP2350_Core0"})
    r.record("probe_rs", "download", 4.0)
    r.count("dap_commands", 12)
    r.probe_start, r.probe_end = snapshot(100, 1), snapshot(250, 3, 2)
    r.finish()
    line = json.loads(json.dumps(r.line()))
    assert line["record"] == "session" and line["source"] == "rig" and line["platform"] == "hardware"
    assert line["bench"] == {"serial": "SER1", "board": "rpi_pi_pico"}
    assert line["dut"] == {"chip": "RP2350_Core0"}
    assert line["operations"][0]["layer"] == "probe_rs" and line["operations"][0]["calls"] == 1
    assert line["counters"] == {"dap_commands": 12}
    assert line["probe"]["delta"] == {"transfers": 150, "errors": 2, "short_replies": 2}
    assert line["start"].startswith("2023-11-14T22:13:20")


def test_discard_takes_every_call_the_rig_makes():
    discard = Discard()
    assert discard.clock() == 0.0
    discard.record("dap", "read", 0.1, "ok", None, words=1)
    discard.reject("dap", "read", "mismatch")
    discard.retry("dap", "read", True)
    discard.count("dap_commands", 3)
    discard.describe(bench={})


class USBError(OSError):
    pass


class USBTimeoutError(USBError):
    pass


def test_error_kinds():
    assert probe_telemetry.error_kind(DapError("x", "wait")) == "wait"
    assert probe_telemetry.error_kind(USBTimeoutError()) == "usb_timeout"
    assert probe_telemetry.error_kind(USBError()) == "usb_error"
    assert probe_telemetry.error_kind(TimeoutError()) == "timeout"
    assert probe_telemetry.error_kind(ValueError()) == "ValueError"
    assert dap.error_kind(struct.error("unpack")) == "short_reply"
    assert dap.error_kind(IndexError()) == "short_reply"


@pytest.mark.parametrize("ack,kind", [(2, "wait"), (4, "fault"), (7, "no_ack"), (0x09, "protocol_error"),
                                      (1, "transfer")])
def test_ack_kinds(ack, kind):
    assert dap.ack_kind(ack) == kind


@pytest.mark.parametrize("text,kind", [
    ("Failed to open probe: USB", "probe_open"),
    ("Error in the USB access", "usb"),
    ("Error handling CMSIS-DAP command Info", "dap_command"),
    ("Verification failed at 0x10000000", "verify"),
    ("", "exit"),
])
def test_probe_rs_kinds(text, kind):
    assert probe_telemetry.probe_rs_kind(text) == kind


def test_a_snapshot_line_is_taken_out_of_the_output():
    output = "usb\nPROBE " + repr(snapshot(5)) + "\nusb\n"
    found, rest = probe_telemetry.parse_snapshot(output)
    assert found == snapshot(5)
    assert rest.split() == ["usb", "usb"]
    found, _ = probe_telemetry.parse_snapshot("PROBE {not python\n")
    assert "error" in found
    assert probe_telemetry.parse_snapshot("usb\n") == (None, "usb")


@pytest.mark.parametrize("start,end,delta", [
    (None, snapshot(40, 1, 0), {"transfers": 40, "errors": 1, "short_replies": 0}),
    ({"board": "b", "probes": []}, snapshot(40), {"transfers": 40, "errors": 0, "short_replies": 0}),
    (snapshot(10), snapshot(40), {"transfers": 30, "errors": 0, "short_replies": 0}),
    (snapshot(50), snapshot(40), {"reset": True}),
    (snapshot(10), None, None),
    ({"error": "boom"}, snapshot(40), None),
])
def test_probe_deltas(start, end, delta):
    assert probe_telemetry.probe_delta(start, end) == delta


def test_a_probe_without_short_replies_counts_the_rest():
    old = {"board": "b", "probes": [{"name": "dut", "transfers": 9, "errors": 0, "short_replies": None}]}
    assert probe_telemetry.probe_delta(None, old) == {"transfers": 9, "errors": 0}


class FakeInstance:
    def __init__(self, model, name, **properties):
        self.model = model
        self.name = name
        for key, value in properties.items():
            setattr(self, key, value)


def test_the_snapshot_code_reads_every_probe_and_tolerates_a_missing_property(monkeypatch):
    instances = [FakeInstance("swd_probe", "dut", enabled=True, clock_hz=1000000, connected=False, transfers=7,
                              errors=1), FakeInstance("bmp388", "baro")]
    monkeypatch.setitem(sys.modules, "metalclaw", SimpleNamespace(instances=lambda: instances))
    monkeypatch.setattr(sys.implementation, "_build", "rpi_pi_pico", raising=False)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        exec(probe_telemetry.PROBE_SNAPSHOT, {})
    found, _ = probe_telemetry.parse_snapshot(out.getvalue())
    assert found == {"board": "rpi_pi_pico", "probes": [{"name": "dut", "enabled": True, "clock_hz": 1000000,
                                                           "connected": False, "transfers": 7, "errors": 1,
                                                           "short_replies": None}]}


def test_the_snapshot_code_prints_its_failure_rather_than_raising(monkeypatch):
    monkeypatch.setitem(sys.modules, "metalclaw", SimpleNamespace())
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        exec(probe_telemetry.PROBE_SNAPSHOT, {})
    found, _ = probe_telemetry.parse_snapshot(out.getvalue())
    assert "error" in found


def test_reading_the_probe_skips_a_gone_harness_and_an_unattached_probe():
    def gone(code, timeout):
        raise RuntimeError("harness SER1 REPL port is gone")

    assert probe_telemetry.read_probe(gone) == (None, "not read: RuntimeError('harness SER1 REPL port is gone')")
    found, note = probe_telemetry.read_probe(lambda code, timeout: "PROBE {'board': 'b', 'probes': []}\n")
    assert found == {"board": "b", "probes": []} and note == "no swd_probe attached"
    found, note = probe_telemetry.read_probe(lambda code, timeout: "PROBE " + repr(snapshot(3)) + "\n")
    assert found == snapshot(3) and note is None


def test_the_end_read_goes_through_the_harness_with_a_short_deadline():
    calls = []
    config = SimpleNamespace(stash={probe_telemetry.RECORDER: recorder()})

    def run(code, timeout):
        calls.append(timeout)
        return "PROBE " + repr(snapshot(3)) + "\n"

    probe_telemetry.read_end(config, SimpleNamespace(run=run))
    assert config.stash[probe_telemetry.RECORDER].probe_end == snapshot(3)
    assert calls == [probe_telemetry.END_READ_TIMEOUT_S]


# DapMemory and _Link


@pytest.fixture
def quiet_link(monkeypatch):
    def opened(self):
        self.commands += 2

    def read_words(self, address, count):
        self.commands += 1
        return [address] * count

    monkeypatch.setattr(dap._Link, "_open", opened)
    monkeypatch.setattr(dap._Link, "read_words", read_words)
    monkeypatch.setattr(dap._Link, "_close", lambda self: None)


def test_a_dap_access_records_its_connect_its_words_and_its_commands(quiet_link):
    r = recorder()
    assert DapMemory("SER1", recorder=r).read_words(0x20000000, 4) == [0x20000000] * 4
    assert r.operations[("dap", "connect")].ok == 1
    read = r.operations[("dap", "read")]
    assert (read.calls, read.ok, read.words) == (1, 1, 4)
    assert r.counters == {"dap_commands": 3}


def test_a_dap_access_that_cannot_connect_records_why_on_both(monkeypatch):
    def missing(self):
        raise DapError("no harness SER1 on USB", "no_harness")

    monkeypatch.setattr(dap._Link, "_open", missing)
    r = recorder()
    with pytest.raises(DapError):
        DapMemory("SER1", recorder=r).read_words(0x20000000, 1)
    assert r.operations[("dap", "connect")].errors == {"no_harness": 1}
    assert r.operations[("dap", "read")].errors == {"no_harness": 1}


def test_a_dap_access_counts_a_usb_timeout(monkeypatch, quiet_link):
    def times_out(self, address, values):
        raise USBTimeoutError("timed out")

    monkeypatch.setattr(dap._Link, "write_words", times_out)
    r = recorder()
    with pytest.raises(USBTimeoutError):
        DapMemory("SER1", recorder=r).write_words(0x20000000, [1, 2])
    assert r.operations[("dap", "write")].errors == {"usb_timeout": 1}


def test_an_invalid_access_raises_before_any_traffic():
    r = recorder()
    with pytest.raises(DapError) as raised:
        DapMemory("SER1", recorder=r).read_words(0x20000001, 1)
    assert raised.value.kind == "invalid"
    assert r.operations == {}


@pytest.mark.parametrize("reply,kind", [(bytes([dap.DAP_TRANSFER, 0, 2]), "wait"),
                                        (bytes([dap.DAP_TRANSFER, 0, 4]), "fault"),
                                        (bytes([dap.DAP_TRANSFER, 0, 7]), "no_ack")])
def test_a_transfer_names_its_ack(monkeypatch, reply, kind):
    link = dap._Link("SER1", 1_000_000, dap.RP2350)
    monkeypatch.setattr(link, "_command", lambda packet: reply)
    with pytest.raises(DapError) as raised:
        link._dp_read(dap.DP_DPIDR)
    assert raised.value.kind == kind


def test_a_block_names_its_ack(monkeypatch):
    link = dap._Link("SER1", 1_000_000, dap.RP2350)
    monkeypatch.setattr(link, "_command", lambda packet: bytes([dap.DAP_TRANSFER_BLOCK, 0, 0, 2]))
    with pytest.raises(DapError) as raised:
        link._block(dap.AP_DRW, 4)
    assert raised.value.kind == "wait"


# ProbeRs


@pytest.fixture
def probe_rs(tmp_path, monkeypatch):
    for name in ("HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "TMPDIR"):
        directory = tmp_path / name.lower()
        directory.mkdir()
        monkeypatch.setenv(name, str(directory))
    r = recorder()
    return ProbeRs("test-probe-id", None, executable=tmp_path / "fake_probe_rs", recorder=r), r


def test_probe_rs_runs_record_their_outcome(probe_rs, monkeypatch):
    probe, r = probe_rs
    replies = [subprocess.CompletedProcess([], 0, "", ""),
               subprocess.CompletedProcess([], 1, "", "Error in the USB access"),
               subprocess.TimeoutExpired([], 30)]

    def run(argv, **kwargs):
        reply = replies.pop(0)
        if isinstance(reply, Exception):
            raise reply
        return reply

    monkeypatch.setattr(subprocess, "run", run)
    probe.reset()
    for _ in range(2):
        with pytest.raises(ProbeError):
            probe.reset()
    reset = r.operations[("probe_rs", "reset")]
    assert (reset.calls, reset.ok, reset.errors) == (3, 1, {"usb": 1, "timeout": 1})


def test_a_probe_rs_read_with_bad_output_is_rejected(probe_rs, monkeypatch):
    probe, r = probe_rs
    monkeypatch.setattr(subprocess, "run", lambda argv, **kwargs: subprocess.CompletedProcess(argv, 0, "zz", ""))
    with pytest.raises(ProbeError) as raised:
        probe.read_words(0x20000000, 1)
    assert raised.value.kind == "bad_output"
    read = r.operations[("probe_rs", "read")]
    assert (read.calls, read.ok, read.errors) == (1, 0, {"bad_output": 1})


# The mailbox's and the DUT's retries


def test_a_mailbox_retry_is_recorded_against_the_memory_layer(monkeypatch):
    monkeypatch.setattr("test.rig.platforms.hardware.mailbox.time.sleep", lambda seconds: None)
    r = recorder()
    box = object.__new__(Mailbox)
    box.memory = SimpleNamespace(LAYER="dap")
    box.recorder = r
    failures = [DapError("DAP transfer stopped after 0 of 1, ack 2", "wait")]

    def call():
        if failures:
            raise failures.pop()
        return [1]

    assert box._access("read", call) == [1]
    operation = r.operations[("dap", "read")]
    assert (operation.retries, operation.recovered) == (1, 1)


def test_a_restored_download_is_a_recovered_retry():
    r = recorder()
    failures = [ProbeError("Failed to erase flash sector", kind="exit")]
    dut = object.__new__(platform.Dut)
    dut.image = Path("/images/smoke/zephyr.elf")
    dut.recorder = r
    dut.mailbox = SimpleNamespace(last_result=3)
    dut.memory = SimpleNamespace(write_when_reachable=lambda address, words, seconds: None,
                                 write_words=lambda address, words: None)
    dut.harness = SimpleNamespace(run=lambda code: "")

    def download(image, reset=True):
        if failures:
            raise failures.pop()

    dut.probe = SimpleNamespace(download=download, reset=lambda: None)
    with pytest.raises(ProbeError):
        dut.flash()
    operation = r.operations[("probe_rs", "download")]
    assert (operation.retries, operation.recovered) == (1, 1)


def test_the_detach_call_carries_the_probe_snapshot_and_still_sees_usb_devices():
    waits = []
    harness = SimpleNamespace(
        usb_interfaces=lambda: ["REPL", "CMSIS-DAP v2", "u-blox"],
        run=lambda code: "usb\nPROBE " + repr(snapshot(11)) + "\n",
        wait_interfaces=lambda settled, what: waits.append(what))
    r = recorder()
    platform.detach_all_but_probe(harness, r)
    assert r.probe_start == snapshot(11)
    assert waits == ["re-enumerate without the detached devices"]
    assert platform.DETACH_ALL_BUT_PROBE.endswith(probe_telemetry.PROBE_SNAPSHOT)


def test_the_detach_call_without_a_usb_device_waits_for_nothing():
    harness = SimpleNamespace(usb_interfaces=lambda: ["REPL"], run=lambda code: "PROBE {'error': 'x'}\n",
                              wait_interfaces=lambda settled, what: pytest.fail("waited"))
    platform.detach_all_but_probe(harness)


# The plugin

USES_THE_PROBE = """
from test.rig import probe_telemetry

def test_uses_the_probe(pytestconfig):
    r = probe_telemetry.recorder(pytestconfig)
    r.describe(bench={"serial": "SER1"})
    r.record("dap", "read", 0.002, words=128)
    r.record("dap", "read", 1.0, "usb_timeout", "timed out")
"""


def test_the_session_line_goes_beside_the_junit_xml(pytester, monkeypatch):
    monkeypatch.delenv(probe_telemetry.LOG_ENV, raising=False)
    monkeypatch.setenv("METALCLAW_PLATFORM", "hardware")
    pytester.makepyfile(USES_THE_PROBE)
    result = pytester.runpytest("-p", "test.rig.probe_telemetry", "--junitxml=results/junit.xml")
    log = pytester.path / "results" / probe_telemetry.LOG_NAME
    result.stdout.fnmatch_lines(["probe: 2 DAP accesses, 1 failed (usb_timeout 1); probe counters: not read: "
                                 "the session never set up the probe; logged to *results/probe_telemetry.jsonl"])
    lines = [json.loads(text) for text in log.read_text().splitlines()]
    assert len(lines) == 1
    assert lines[0]["platform"] == "hardware" and lines[0]["bench"] == {"serial": "SER1"}
    assert lines[0]["exitstatus"] == 0
    assert lines[0]["operations"][0]["errors"] == {"usb_timeout": 1}


def test_the_environment_names_the_log_and_sessions_append(pytester, monkeypatch, tmp_path):
    log = tmp_path / "logs" / "probe.jsonl"
    monkeypatch.setenv(probe_telemetry.LOG_ENV, str(log))
    pytester.makepyfile(USES_THE_PROBE)
    pytester.runpytest("-p", "test.rig.probe_telemetry", "--junitxml=results/junit.xml")
    pytester.runpytest("-p", "test.rig.probe_telemetry")
    assert len(log.read_text().splitlines()) == 2
    assert not (pytester.path / "results" / probe_telemetry.LOG_NAME).exists()


def test_without_a_log_path_nothing_is_written(pytester, monkeypatch):
    monkeypatch.delenv(probe_telemetry.LOG_ENV, raising=False)
    pytester.makepyfile(USES_THE_PROBE)
    result = pytester.runpytest("-p", "test.rig.probe_telemetry")
    result.stdout.fnmatch_lines(["probe: * not logged (set METALCLAW_PROBE_LOG or --junitxml)"])
    assert not list(pytester.path.rglob("*.jsonl"))


def test_a_session_that_never_touches_the_probe_writes_nothing(pytester, monkeypatch):
    monkeypatch.delenv(probe_telemetry.LOG_ENV, raising=False)
    pytester.makepyfile("def test_nothing(): pass\n")
    result = pytester.runpytest("-p", "test.rig.probe_telemetry", "--junitxml=junit.xml")
    assert "probe:" not in result.stdout.str()
    assert not (pytester.path / probe_telemetry.LOG_NAME).exists()
