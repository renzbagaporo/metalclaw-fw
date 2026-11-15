# SPDX-License-Identifier: Apache-2.0 OR MIT

import argparse
import json
import struct
from pathlib import Path
from types import SimpleNamespace

import pytest

from test.rig import probe_soak
from test.rig.platforms.hardware import mailbox
from test.rig.platforms.hardware.dap import DapError
from test.rig.platforms.hardware.probe import ProbeError
from test.rig.probe_soak import SoakError
from test.rig.probe_telemetry import Recorder

FLASH = 0x10000000
SCRATCH = 0x20001640


def recorder():
    return Recorder("soak", clock=lambda: 0.0, wall=lambda: 1_700_000_000.0)


@pytest.mark.parametrize("text,seconds", [("90", 90.0), ("90s", 90.0), ("30m", 1800.0), ("1h", 3600.0),
                                          ("1.5h", 5400.0)])
def test_durations(text, seconds):
    assert probe_soak.parse_duration(text) == seconds


@pytest.mark.parametrize("text", ["", "h", "-5m", "0", "soon"])
def test_bad_durations(text):
    with pytest.raises(argparse.ArgumentTypeError):
        probe_soak.parse_duration(text)


def test_scratch_arguments():
    assert probe_soak.parse_scratch("0x20040000:1024") == (0x20040000, 1024)
    assert probe_soak.parse_scratch("0x20040000:0x400") == (0x20040000, 1024)
    with pytest.raises(argparse.ArgumentTypeError):
        probe_soak.parse_scratch("0x20040000")


def test_patterns_repeat_by_seed_and_change_every_iteration():
    first = probe_soak.pattern(7, 0, 64)
    assert first == probe_soak.pattern(7, 0, 64)
    assert first != probe_soak.pattern(7, 1, 64)
    assert first != probe_soak.pattern(8, 0, 64)
    assert all(0 <= word < 1 << 32 for word in first)
    assert len(set(first)) > 60


def test_compare_names_every_differing_word():
    assert probe_soak.compare(0x100, [1, 2, 3], [1, 2, 3]) == []
    assert probe_soak.compare(0x100, [1, 2, 3], [1, 9, 3]) == [(0x104, 2, 9)]
    assert probe_soak.compare(0x100, [1, 2, 3], [1]) == [(0x104, 2, None), (0x108, 3, None)]
    text = probe_soak.describe_differences([(0x104, 2, 9), (0x108, 3, None)], shown=1)
    assert text == "2 words differ: 0x00000104: wrote 0x00000002 read 0x00000009, and 1 more"


def test_blocks_go_round_the_scratch_area():
    found = probe_soak.blocks(0x1000, 40, 4)
    assert [next(found) for _ in range(4)] == [(0x1000, 4), (0x1010, 4), (0x1020, 2), (0x1000, 4)]
    with pytest.raises(SoakError):
        next(probe_soak.blocks(0x1000, 2, 4))


def test_flash_windows_cover_whole_words_of_each_segment():
    segments = [(FLASH + 2, bytes(range(14))), (FLASH + 0x100, struct.pack("<3I", 7, 8, 9))]
    found = probe_soak.flash_windows(segments, 2)
    assert [next(found) for _ in range(5)] == [
        (FLASH + 4, [int.from_bytes(bytes([2, 3, 4, 5]), "little"), int.from_bytes(bytes([6, 7, 8, 9]), "little")]),
        (FLASH + 12, [int.from_bytes(bytes([10, 11, 12, 13]), "little")]),
        (FLASH + 0x100, [7, 8]),
        (FLASH + 0x108, [9]),
        (FLASH + 4, [int.from_bytes(bytes([2, 3, 4, 5]), "little"), int.from_bytes(bytes([6, 7, 8, 9]), "little")]),
    ]


NM = """20001640 00000400 B mclw_dut_scratch
200001a8 00000200 B mclw_dut_mailbox_result
20000020 00000010 d some_data
20001a40 B _end
10000c54 0000007c T mclw_dut_mailbox_init
garbage line
"""


def test_nm_output():
    symbols = probe_soak.parse_nm(NM)
    assert symbols["mclw_dut_scratch"] == (0x20001640, 0x400, "B")
    assert symbols["_end"] == (0x20001A40, None, "B")
    assert "garbage" not in symbols and "line" not in symbols


ARENA = "CONFIG_SOC_SERIES=\"rp2350\"\nCONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=-1\n"


def test_the_scratch_area_is_the_images_scratch_symbol():
    assert probe_soak.scratch_area(None, probe_soak.parse_nm(NM), "rp2350", ARENA) == (0x20001640, 0x400)


def test_without_a_scratch_symbol_or_argument_the_soak_refuses():
    symbols = probe_soak.parse_nm(NM)
    del symbols["mclw_dut_scratch"]
    with pytest.raises(SoakError, match="no mclw_dut_scratch"):
        probe_soak.scratch_area(None, symbols, "rp2350", ARENA)


@pytest.mark.parametrize("given,why", [
    ((0x20000100, 0x200), "overlaps the image's mclw_dut_mailbox_result"),
    ((0x20040000, 0x100), "malloc arena"),
    ((0x20081F00, 0x200), "not in the DUT's SRAM"),
    ((0x20000002, 0x10), "not whole aligned words"),
    ((0x20000000, 0), "not whole aligned words"),
])
def test_a_given_scratch_area_must_hold_nothing_of_the_image(given, why):
    with pytest.raises(SoakError, match=why):
        probe_soak.scratch_area(given, probe_soak.parse_nm(NM), "rp2350", ARENA)


def test_a_given_scratch_area_in_free_ram_is_used():
    assert probe_soak.scratch_area((0x20040000, 0x100), probe_soak.parse_nm(NM), "rp2350", "") == (0x20040000, 0x100)


def test_a_given_scratch_area_stays_out_of_a_micropython_heap():
    symbols = {"__GcHeapStart": (0x20010000, None, "B"), "__GcHeapEnd": (0x20030000, None, "B")}
    with pytest.raises(SoakError, match="MicroPython heap"):
        probe_soak.scratch_area((0x20020000, 0x100), symbols, "rp2040")


class Log(probe_soak.Log):
    def __init__(self):
        self.lines = []
        self.events = 0
        self.suppressed = 0

    def write(self, record):
        self.lines.append(json.loads(json.dumps(record)))


class FakeProbe:
    def __init__(self, failures=()):
        self.calls = []
        self.failures = list(failures)

    def download(self, image, verify, reset):
        assert verify and not reset
        self.calls.append(("download", image))
        if self.failures and self.failures[0] == len(self.calls):
            self.failures.pop(0)
            raise ProbeError("Error in the USB access", kind="usb")

    def reset(self):
        self.calls.append(("reset",))


def test_the_flash_soak_alternates_images_and_retries_a_failed_step():
    probe = FakeProbe(failures=[3])
    r, log = recorder(), Log()
    state = {"cycles": 0, "failed_cycles": 0}
    probe_soak.flash_soak(probe, [Path("a.elf"), Path("b.elf")], 3, 2, 5, r, log, state)
    assert [call[1].name for call in probe.calls if call[0] == "download"] == ["a.elf", "b.elf", "b.elf", "a.elf"]
    assert state == {"cycles": 3, "failed_cycles": 0, "flashed": Path("a.elf")}
    cycles = [line for line in log.lines if line["record"] == "cycle"]
    assert [line["download"]["attempts"] for line in cycles] == [1, 2, 1]
    assert cycles[1]["download"]["errors"][0]["kind"] == "usb"
    assert (r.operations[("probe_rs", "download")].retries, r.operations[("probe_rs", "download")].recovered) == (1, 1)
    assert r.operations[("soak", "flash_cycle")].ok == 3


def test_the_flash_soak_stops_when_cycles_keep_failing():
    probe = FakeProbe(failures=list(range(1, 100)))
    r, log = recorder(), Log()
    state = {"cycles": 0, "failed_cycles": 0}
    probe_soak.flash_soak(probe, [Path("a.elf"), Path("b.elf")], 100, 1, 3, r, log, state)
    assert state["cycles"] == 3 and state["failed_cycles"] == 3
    assert state["stopped"] == "3 cycles in a row failed" and state["flashed"] is None
    assert r.operations[("soak", "flash_cycle")].errors == {"usb": 3}


class FakeMemory:
    LAYER = "dap"

    def __init__(self, flash=None, corrupt_every=0, fail=()):
        self.ram = {}
        self.flash = flash or {}
        self.reads = 0
        self.corrupt_every = corrupt_every
        self.fail = set(fail)

    def write_words(self, address, words):
        for index, word in enumerate(words):
            self.ram[address + 4 * index] = word

    def read_words(self, address, count):
        self.reads += 1
        if self.reads in self.fail:
            raise DapError("DAP transfer stopped after 0 of 1, ack 2", "wait")
        source = self.flash if address < 0x20000000 else self.ram
        words = [source.get(address + 4 * index, 0) for index in range(count)]
        if self.corrupt_every and self.reads % self.corrupt_every == 0:
            words[0] ^= 1
        return words


class Ticks:
    def __init__(self, step=1.0):
        self.now = 0.0
        self.step = step

    def __call__(self):
        self.now += self.step
        return self.now


def soak_memory(memory, iterations, **options):
    r, log = recorder(), Log()
    state = {"started": 0.0, "iterations": 0, "mismatched_words": 0, "stopped": None}
    segments = [(FLASH, struct.pack("<8I", *range(8)))]
    settings = dict(scratch=(SCRATCH, 64), windows=probe_soak.flash_windows(segments, 4), block_words=8,
                    flash_every=1, duration=iterations + 0.5, seed=1, max_consecutive=3)
    settings.update(options)
    probe_soak.memory_soak(memory, r, log, state, clock=Ticks(), sleep=lambda seconds: None, **settings)
    return r, log, state


def flash_of(words):
    return {FLASH + 4 * index: word for index, word in enumerate(words)}


def test_a_clean_memory_soak_counts_every_cycle():
    r, log, state = soak_memory(FakeMemory(flash_of(range(8))), 10)
    assert state["iterations"] == 10
    assert r.operations[("soak", "sram_cycle")].ok == 10
    assert r.operations[("soak", "flash_cycle")].ok == 10
    assert log.lines == []


def test_a_mismatch_is_counted_and_logged_with_its_words():
    r, log, state = soak_memory(FakeMemory(flash_of(range(8)), corrupt_every=4), 10)
    counted = sum(r.operations[("soak", name)].errors.get("mismatch", 0) for name in ("sram_cycle", "flash_cycle"))
    # Two reads an iteration, every fourth read corrupted.
    assert counted == state["iterations"] * 2 // 4 == len(log.lines) == state["mismatched_words"] > 0
    assert all(line["kind"] == "mismatch" and "1 words differ" in line["message"] for line in log.lines)


def test_access_errors_are_counted_by_kind_and_a_dead_bench_stops_the_soak():
    r, log, state = soak_memory(FakeMemory(flash_of(range(8)), fail=range(1, 1000)), 50)
    assert state["stopped"] == "3 iterations in a row could not reach the DUT"
    assert r.operations[("soak", "sram_cycle")].errors == {"wait": 3}
    assert [line["kind"] for line in log.lines] == ["wait"] * 6


def test_a_read_only_soak_writes_nothing():
    memory = FakeMemory(flash_of(range(8)))
    r, _, _ = soak_memory(memory, 5, scratch=None)
    assert memory.ram == {}
    assert ("soak", "sram_cycle") not in r.operations
    assert r.operations[("soak", "flash_cycle")].ok == 5


def test_the_log_caps_its_events(tmp_path, monkeypatch):
    monkeypatch.setattr(probe_soak, "EVENTS", 2)
    log = probe_soak.Log(tmp_path / "soak.jsonl")
    for index in range(5):
        log.event({"index": index})
    log.close()
    assert len((tmp_path / "soak.jsonl").read_text().splitlines()) == 2
    assert log.suppressed == 3


def running(memory, header=probe_soak.MAILBOX_HEADER, halted=False):
    for index, word in enumerate(header):
        memory.ram[0x200001A8 + 4 * index] = word
    memory.ram[probe_soak.DHCSR] = 1 << 17 if halted else 0
    return memory


SYMBOLS = {mailbox.RESULT_SYMBOL: (0x200001A8, 0x200, "B")}
SEGMENTS = [(FLASH, struct.pack("<8I", *range(8)))]


def test_a_running_image_passes_the_check():
    state = {}
    probe_soak.check_running(running(FakeMemory(flash_of(range(8)))), Path("smoke.elf"), SYMBOLS, SEGMENTS, 4, state)
    assert state == {}


def test_a_dut_holding_another_image_is_refused():
    with pytest.raises(SoakError, match="does not hold"):
        probe_soak.check_running(running(FakeMemory(flash_of([9] * 8))), Path("smoke.elf"), SYMBOLS, SEGMENTS, 4, {})


def test_a_dut_whose_mailbox_is_not_up_is_refused():
    with pytest.raises(SoakError, match="not running"):
        probe_soak.check_running(running(FakeMemory(flash_of(range(8))), header=(0, 0, 0, 0)), Path("smoke.elf"),
                                 SYMBOLS, SEGMENTS, 4, {})


def test_a_halted_dut_is_resumed():
    memory = running(FakeMemory(flash_of(range(8))), halted=True)
    state = {}
    probe_soak.check_running(memory, Path("smoke.elf"), SYMBOLS, SEGMENTS, 4, state)
    assert memory.ram[probe_soak.DHCSR] == probe_soak.DHCSR_RESUME
    assert state == {"resumed_halted_core": True}


class FakePylink:
    class JLinkException(Exception):
        pass

    enums = SimpleNamespace(JLinkInterfaces=SimpleNamespace(SWD="swd"))

    def __init__(self):
        self.links = []
        self.fail_reads = 0
        pylink = self

        class JLink:
            def __init__(self):
                self.calls = []
                pylink.links.append(self)

            def open(self, serial_no):
                self.calls.append(("open", serial_no))

            def set_tif(self, interface):
                self.calls.append(("tif", interface))

            def connect(self, device, speed):
                self.calls.append(("connect", device, speed))

            def memory_read32(self, address, count):
                if pylink.fail_reads:
                    pylink.fail_reads -= 1
                    raise pylink.JLinkException("read failed")
                return [address] * count

            def memory_write32(self, address, words):
                self.calls.append(("write", address, list(words)))

            def close(self):
                self.calls.append(("close",))

        self.JLink = JLink


def test_the_jlink_memory_keeps_one_connection_and_makes_another_after_a_failure():
    pylink = FakePylink()
    r = recorder()
    memory = probe_soak.JLinkMemory("000123456789", "RP2350_M33_0", 4000, r, pylink)
    assert memory.read_words(0x20000000, 2) == [0x20000000] * 2
    memory.write_words(0x20000000, [1])
    pylink.fail_reads = 1
    with pytest.raises(FakePylink.JLinkException):
        memory.read_words(0x20000000, 1)
    memory.read_words(0x20000000, 1)
    memory.close()
    assert len(pylink.links) == 2
    assert pylink.links[0].calls[:3] == [("open", 123456789), ("tif", "swd"), ("connect", "RP2350_M33_0", 4000)]
    assert r.operations[("jlink", "connect")].ok == 2
    assert r.operations[("jlink", "read")].errors == {"JLinkException": 1}
    assert r.operations[("jlink", "write")].words == 1


# The command line, with the bench faked


def elf(segments):
    phoff = 52
    data_offset = phoff + len(segments) * 32
    headers, payload = bytearray(), bytearray()
    for physical, contents in segments:
        headers += struct.pack("<8I", 1, data_offset + len(payload), physical, physical, len(contents),
                               len(contents), 5, 4)
        payload += contents
    ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    return ident + struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, FLASH | 1, phoff, 0, 0, 52, 32, len(segments),
                               40, 0, 0) + bytes(headers) + bytes(payload)


def image(directory, words):
    directory.mkdir()
    (directory / ".config").write_text(ARENA)
    path = directory / "zephyr.elf"
    path.write_bytes(elf([(FLASH, struct.pack(f"<{len(words)}I", *words))]))
    return path


class FakeBench:
    def __init__(self, serial, attach):
        self.serial = serial
        self.reads = 0

    def snapshot(self):
        self.reads += 1
        return {"board": "fixture1_pico", "probes": [{"name": "dut", "transfers": 100 * self.reads, "errors": 0,
                                                      "short_replies": 0}]}, None

    def close(self):
        pass


@pytest.fixture
def bench(monkeypatch, tmp_path):
    monkeypatch.setattr(probe_soak, "Bench", FakeBench)
    monkeypatch.setattr(probe_soak, "image_symbols", lambda image, toolchain: {
        **probe_soak.parse_nm(NM), mailbox.RESULT_SYMBOL: (0x200001A8, 0x200, "B")})
    monkeypatch.setattr(probe_soak, "Progress", lambda: (lambda text_of: None))
    monkeypatch.setattr(probe_soak.signal, "signal", lambda signum, handler: None)
    return tmp_path


def lines(path):
    return [json.loads(text) for text in path.read_text().splitlines()]


def test_an_interrupted_memory_soak_still_writes_its_session_and_summary(bench, monkeypatch, capsys):
    smoke = image(bench / "smoke", range(64))

    class Interrupting(FakeMemory):
        def __init__(self, serial, debug_port, clock_hz=0, recorder=None):
            super().__init__(flash_of(range(64)))
            running(self)
            self.recorder = recorder

        def read_words(self, address, count):
            if self.reads == 40:
                raise KeyboardInterrupt
            words = super().read_words(address, count)
            self.recorder.record("dap", "read", 0.001, words=count)
            return words

    monkeypatch.setattr(probe_soak.dap, "DapMemory", Interrupting)
    log = bench / "soak.jsonl"
    status = probe_soak.main(["memory", "--serial", "E661", "--image", str(smoke), "--log", str(log),
                              "--duration", "1h", "--seed", "5"])
    assert status == 130
    session = lines(log)[-1]
    assert session["record"] == "session" and session["source"] == "soak"
    assert session["soak"]["mode"] == "memory" and session["soak"]["interrupted"] is True
    assert session["soak"]["scratch"] == {"address": "0x20001640", "bytes": 1024}
    assert session["soak"]["iterations"] > 0 and session["soak"]["seed"] == 5
    assert session["bench"] == {"serial": "E661", "board": "fixture1_pico"}
    assert session["probe"]["delta"] == {"transfers": 100, "errors": 0, "short_replies": 0}
    out = capsys.readouterr()
    assert "hardware E661 fixture1_pico [soak memory]: 1 session" in out.out
    assert "interrupted; the summary is partial" in out.err


def test_the_flash_soak_refuses_two_images_with_the_same_flash(bench, capsys):
    first = image(bench / "a", range(16))
    second = image(bench / "b", range(16))
    log = bench / "soak.jsonl"
    status = probe_soak.main(["flash", "--serial", "E661", "--image", str(first), "--image", str(second),
                              "--log", str(log)])
    assert status == 2
    assert "same bytes in flash" in capsys.readouterr().err
    assert lines(log)[-1]["soak"]["stopped"].startswith("refused or failed")


def test_the_flash_soak_runs_its_cycles_and_restores_the_first_image(bench, monkeypatch):
    first = image(bench / "a", range(16))
    second = image(bench / "b", range(1, 17))
    probe = FakeProbe()
    monkeypatch.setattr(probe_soak, "_harness_probe", lambda serial, chip, speed, recorder: probe)
    log = bench / "soak.jsonl"
    assert probe_soak.main(["flash", "--serial", "E661", "--image", str(first), "--image", str(second),
                            "--cycles", "4", "--log", str(log)]) == 0
    downloads = [call[1] for call in probe.calls if call[0] == "download"]
    assert downloads == [first, second, first, second, first]
    records = lines(log)
    assert [record["record"] for record in records] == ["cycle"] * 4 + ["session"]
    assert records[-1]["soak"]["cycles"] == 4 and records[-1]["soak"]["restored"] == str(first)


def test_a_soak_needs_a_log(monkeypatch, capsys):
    monkeypatch.delenv("METALCLAW_PROBE_LOG", raising=False)
    assert probe_soak.main(["memory", "--serial", "E661", "--image", "x.elf"]) == 2
    assert "give --log" in capsys.readouterr().err


def test_a_dut_the_soak_cannot_reach_stops_it_with_its_session_line(bench, monkeypatch, capsys):
    smoke = image(bench / "smoke", range(64))

    class Unreachable(FakeMemory):
        def __init__(self, serial, debug_port, clock_hz=0, recorder=None):
            super().__init__()

        def read_words(self, address, count):
            raise DapError("no harness E661 on USB", "no_harness")

    monkeypatch.setattr(probe_soak.dap, "DapMemory", Unreachable)
    log = bench / "soak.jsonl"
    assert probe_soak.main(["memory", "--serial", "E661", "--image", str(smoke), "--log", str(log)]) == 2
    assert lines(log)[-1]["soak"]["stopped"] == "failed: DapError('no harness E661 on USB')"
    assert "no harness E661 on USB" in capsys.readouterr().err


def test_the_soak_never_flashes_through_a_jlink(bench, capsys):
    smoke = image(bench / "smoke", range(64))
    log = bench / "soak.jsonl"
    assert probe_soak.main(["memory", "--jlink", "000123456789", "--flash", "--image", str(smoke),
                            "--log", str(log)]) == 2
    assert "never flashes through a J-Link" in capsys.readouterr().err
