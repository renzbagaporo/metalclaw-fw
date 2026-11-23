#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Rolls up the probe telemetry lines that rig sessions (test/rig/probe_telemetry.py)
# and probe soaks (test/rig/probe_soak.py) write: per bench and per operation,
# the calls, the error and retry rates, and the duration percentiles.
#
#   test/rig/probe_report.py 'build/probe/*.jsonl' ~/logs/probe.jsonl
#   test/rig/probe_report.py --source soak --since 2026-11-01 probe.jsonl
#
# Paths may be globs, quoted so the shell leaves them alone. Lines that are
# not session records (a soak's per-cycle lines) are skipped.

import argparse
import glob
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from test.rig.probe_telemetry import PROBE_COUNTERS, SESSION, Operation, probe_delta  # noqa: E402

PERCENTILES = (0.5, 0.95, 0.99)


@dataclass
class Bench:
    key: str
    sessions: int = 0
    first: str | None = None
    last: str | None = None
    seconds: float = 0.0
    operations: dict = field(default_factory=dict)
    probe: dict = field(default_factory=dict)
    probe_sessions: int = 0
    probe_resets: int = 0

    def add(self, line: dict) -> None:
        self.sessions += 1
        start, end = line.get("start"), line.get("end")
        if start and (self.first is None or start < self.first):
            self.first = start
        if end and (self.last is None or end > self.last):
            self.last = end
        self.seconds += float(line.get("seconds") or 0.0)
        for data in line.get("operations", []):
            operation = Operation.from_json(data)
            key = (operation.layer, operation.name)
            if key in self.operations:
                self.operations[key].merge(operation)
            else:
                self.operations[key] = operation
        probe = line.get("probe") or {}
        delta = probe.get("delta")
        if delta is None:
            delta = probe_delta(probe.get("start"), probe.get("end"))
        if delta and delta.get("reset"):
            self.probe_resets += 1
        elif delta:
            self.probe_sessions += 1
            for name in PROBE_COUNTERS:
                if name in delta:
                    self.probe[name] = self.probe.get(name, 0) + int(delta[name])


def source_label(line: dict) -> str:
    source = str(line.get("source", "?"))
    mode = (line.get("soak") or {}).get("mode")
    return f"{source} {mode}" if mode else source


# Rig sessions and each kind of soak are kept apart: a soak's millions of
# accesses would hide the rig's own rates.
def bench_key(line: dict) -> str:
    bench = line.get("bench") or {}
    parts = [str(line.get("platform") or "?"), str(bench.get("serial") or "?"), str(bench.get("board") or "?"),
             f"[{source_label(line)}]"]
    probe = (line.get("soak") or {}).get("probe_id")
    if probe:
        parts.append(f"via {probe}")
    return " ".join(parts)


def expand(patterns: list[str]) -> tuple[list[Path], list[str]]:
    paths, missing = [], []
    for pattern in patterns:
        found = sorted(glob.glob(str(Path(pattern).expanduser()), recursive=True))
        if not found:
            missing.append(pattern)
        paths.extend(Path(path) for path in found)
    return paths, missing


def read_sessions(paths: list[Path]) -> tuple[list[dict], int]:
    sessions, malformed = [], 0
    for path in paths:
        with path.open() as log:
            for text in log:
                text = text.strip()
                if not text:
                    continue
                try:
                    line = json.loads(text)
                except json.JSONDecodeError:
                    malformed += 1
                    continue
                if isinstance(line, dict) and line.get("record") == SESSION:
                    sessions.append(line)
    return sessions, malformed


def select(sessions: list[dict], source=None, platform=None, since=None) -> list[dict]:
    kept = []
    for line in sessions:
        if source and line.get("source") != source:
            continue
        if platform and line.get("platform") != platform:
            continue
        if since and (line.get("start") or "") < since:
            continue
        kept.append(line)
    return kept


def roll_up(sessions: list[dict]) -> dict[str, Bench]:
    benches: dict[str, Bench] = {}
    for line in sessions:
        key = bench_key(line)
        benches.setdefault(key, Bench(key)).add(line)
    return benches


def duration(seconds: float | None) -> str:
    if seconds is None:
        return "-"
    if seconds < 1e-3:
        return f"{seconds * 1e6:.0f}us"
    if seconds < 1.0:
        return f"{seconds * 1e3:.1f}ms"
    if seconds < 600.0:
        return f"{seconds:.2f}s"
    return f"{seconds / 60:.1f}min"


# A rate too small for three decimals of a percent reads as one in so many.
def rate(part: int, whole: int) -> str:
    if not whole:
        return "-"
    percent = 100.0 * part / whole
    if part and percent < 0.001:
        return f"1 in {round(whole / part)}"
    return f"{percent:.3f}%"


def minute(stamp: str | None) -> str:
    return stamp[:16].replace("T", " ") if stamp else "?"


COLUMNS = ("layer", "operation", "calls", "errors", "error%", "retries", "retry%", "recovered",
           "p50", "p95", "p99", "max", "errors by kind")


def operation_row(operation: Operation) -> list[str]:
    errors = sum(operation.errors.values())
    timing = operation.timing
    return [operation.layer, operation.name, str(operation.calls), str(errors), rate(errors, operation.calls),
            str(operation.retries), rate(operation.retries, operation.calls), str(operation.recovered),
            *(duration(timing.percentile(q)) for q in PERCENTILES), duration(timing.max if timing.count else None),
            ", ".join(f"{kind} {count}" for kind, count in sorted(operation.errors.items(), key=lambda kv: -kv[1]))]


def table(rows: list[list[str]], indent: str = "  ") -> list[str]:
    widths = [max(len(row[column]) for row in rows) for column in range(len(rows[0]))]
    lines = []
    for row in rows:
        cells = []
        for column, cell in enumerate(row):
            last = column == len(row) - 1
            numeric = 2 <= column < len(row) - 1
            cells.append(cell if last else cell.rjust(widths[column]) if numeric else cell.ljust(widths[column]))
        lines.append((indent + "  ".join(cells)).rstrip())
    return lines


def render_bench(bench: Bench) -> list[str]:
    lines = [f"{bench.key}: {bench.sessions} session{'s' if bench.sessions != 1 else ''}, "
             f"{minute(bench.first)} .. {minute(bench.last)} UTC, {duration(bench.seconds)} in all"]
    if bench.operations:
        rows = [list(COLUMNS)] + [operation_row(operation) for _, operation in sorted(bench.operations.items())]
        lines += table(rows)
    else:
        lines.append("  no probe operations recorded")
    if bench.probe_sessions:
        transfers = bench.probe.get("transfers", 0)
        errors = bench.probe.get("errors", 0)
        short = bench.probe.get("short_replies", 0)
        lines.append(f"  harness probe: {transfers} transfers, {errors} wire errors ({rate(errors, transfers)}), "
                     f"{short} short replies, over {bench.probe_sessions} of {bench.sessions} sessions")
    else:
        lines.append(f"  harness probe: no counters read in {bench.sessions} sessions")
    if bench.probe_resets:
        lines.append(f"  harness probe counters went down in {bench.probe_resets} sessions, which are left out")
    return lines


def render(sessions: list[dict]) -> str:
    if not sessions:
        return "no probe telemetry sessions"
    lines = []
    for _, bench in sorted(roll_up(sessions).items()):
        if lines:
            lines.append("")
        lines += render_bench(bench)
    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Roll up probe telemetry JSON lines per bench and operation.")
    parser.add_argument("paths", nargs="+", help="JSON-lines files or globs")
    parser.add_argument("--source", choices=("rig", "soak"), help="only sessions from the rig, or only soaks")
    parser.add_argument("--platform", help="only this platform (hardware)")
    parser.add_argument("--since", help="only sessions that started at or after this ISO time, e.g. 2026-11-01")
    args = parser.parse_args(argv)
    paths, missing = expand(args.paths)
    for pattern in missing:
        print(f"probe_report: nothing matches {pattern}", file=sys.stderr)
    sessions, malformed = read_sessions(paths)
    if malformed:
        print(f"probe_report: skipped {malformed} malformed lines", file=sys.stderr)
    print(render(select(sessions, args.source, args.platform, args.since)))
    return 0 if paths else 1


if __name__ == "__main__":
    sys.exit(main())
