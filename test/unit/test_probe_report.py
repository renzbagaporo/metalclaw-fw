# SPDX-License-Identifier: Apache-2.0 OR MIT

import json

from test.rig import probe_report
from test.rig.probe_telemetry import Recorder


def session(serial="E661", source="rig", reads=(0.002,) * 10, failures=0, retries=0, transfers=(0, 100),
            mode=None, start=1_700_000_000.0):
    r = Recorder(source, clock=lambda: 0.0, wall=lambda: start)
    r.describe(platform="hardware", bench={"serial": serial})
    if mode:
        r.describe(soak={"mode": mode})
    r.record("probe_rs", "download", 4.0)
    for seconds in reads:
        r.record("dap", "read", seconds, words=128)
    for _ in range(failures):
        r.record("dap", "read", 1.0, "usb_timeout", "timed out")
    for _ in range(retries):
        r.retry("dap", "read", True)
    before, after = transfers
    r.probe_start = {"board": "fixture1_pico", "probes": [{"name": "dut", "transfers": before, "errors": 0,
                                                           "short_replies": 0}]}
    r.probe_end = {"board": "fixture1_pico", "probes": [{"name": "dut", "transfers": after, "errors": 1,
                                                         "short_replies": 0}]}
    r.finish()
    return r.line()


def write(path, *lines):
    path.write_text("".join(json.dumps(line) + "\n" for line in lines))


def test_sessions_of_a_bench_add_up(tmp_path, capsys):
    write(tmp_path / "a.jsonl", session(failures=1, retries=1), session(start=1_700_000_100.0))
    write(tmp_path / "b.jsonl", session(serial="D4D2"))
    assert probe_report.main([str(tmp_path / "*.jsonl")]) == 0
    out = capsys.readouterr().out
    assert "hardware D4D2 fixture1_pico [rig]: 1 session," in out
    assert "hardware E661 fixture1_pico [rig]: 2 sessions," in out
    rows = [line.split() for line in out.splitlines() if line.strip().startswith("dap")]
    # E661 sorts after D4D2: 21 reads, 1 failed, 1 retried.
    assert rows[1][:8] == ["dap", "read", "21", "1", "4.762%", "1", "4.762%", "1"]
    assert rows[1][-2:] == ["usb_timeout", "1"]
    assert "harness probe: 200 transfers, 2 wire errors (1.000%), 0 short replies, over 2 of 2 sessions" in out


def test_the_percentiles_come_from_the_merged_durations():
    benches = probe_report.roll_up([session(reads=[0.001] * 98 + [0.5, 0.9])])
    read = next(iter(benches.values())).operations[("dap", "read")]
    row = probe_report.operation_row(read)
    p50, p95, p99, largest = row[8:12]
    assert p50.endswith("ms") and 1.0 <= float(p50[:-2]) <= 1.1
    assert p95 == p50
    assert p99.endswith("ms") and 500 <= float(p99[:-2]) <= 546
    assert largest == "900.0ms"


def test_rig_sessions_and_soaks_are_kept_apart():
    benches = probe_report.roll_up([session(), session(source="soak", mode="memory")])
    assert sorted(benches) == ["hardware E661 fixture1_pico [rig]", "hardware E661 fixture1_pico [soak memory]"]


def test_a_soak_through_another_probe_is_its_own_group():
    line = session(source="soak", mode="memory")
    line["soak"]["probe_id"] = "1366:0101:000123456789"
    assert probe_report.bench_key(line) == "hardware E661 fixture1_pico [soak memory] via 1366:0101:000123456789"


def test_other_records_and_malformed_lines_are_skipped(tmp_path, capsys):
    path = tmp_path / "soak.jsonl"
    path.write_text(json.dumps({"record": "cycle", "cycle": 0}) + "\n{broken\n\n" + json.dumps(session()) + "\n")
    probe_report.main([str(path)])
    captured = capsys.readouterr()
    assert "1 session," in captured.out
    assert "skipped 1 malformed lines" in captured.err


def test_counters_that_went_down_are_left_out(capsys, tmp_path):
    write(tmp_path / "a.jsonl", session(transfers=(500, 100)))
    probe_report.main([str(tmp_path / "a.jsonl")])
    out = capsys.readouterr().out
    assert "harness probe: no counters read in 1 sessions" in out
    assert "counters went down in 1 sessions" in out


def test_filters(tmp_path, capsys):
    write(tmp_path / "a.jsonl", session(), session(source="soak", mode="flash", start=1_800_000_000.0))
    probe_report.main(["--source", "soak", str(tmp_path / "a.jsonl")])
    assert "[rig]" not in capsys.readouterr().out
    probe_report.main(["--since", "2027-01-01", str(tmp_path / "a.jsonl")])
    out = capsys.readouterr().out
    assert "[soak flash]" in out and "[rig]" not in out
    probe_report.main(["--platform", "emulator", str(tmp_path / "a.jsonl")])
    assert capsys.readouterr().out.strip() == "no probe telemetry sessions"


def test_a_pattern_that_matches_nothing_is_reported(tmp_path, capsys):
    assert probe_report.main([str(tmp_path / "none-*.jsonl")]) == 1
    assert "nothing matches" in capsys.readouterr().err


def test_rates():
    assert [probe_report.rate(*pair) for pair in ((0, 0), (0, 10), (1, 3), (2, 1_000_000))] == [
        "-", "0.000%", "33.333%", "1 in 500000"]


def test_durations_read_at_a_glance():
    assert [probe_report.duration(s) for s in (None, 0.00042, 0.0123, 4.2, 3600.0)] == [
        "-", "420us", "12.3ms", "4.20s", "60.0min"]
