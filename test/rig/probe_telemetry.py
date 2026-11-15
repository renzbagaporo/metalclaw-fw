# SPDX-License-Identifier: Apache-2.0 OR MIT
# How the SWD probe behaved in a rig session: every probe-rs run and every
# DAP access the rig made, with its outcome, retries and duration, and the
# harness probe's own counters. It is kept in memory and written as one JSON
# line when the session ends, so a probe that fails one access in ten
# thousand shows as a rate over many sessions (test/rig/probe_report.py)
# rather than as one unexplained failure.
#
# Recording costs two clock reads and a few dict updates per access. The only
# traffic it adds is one REPL call to the harness after the last case; the
# counters at the start ride on the REPL call the harness fixture makes anyway.

import ast
import json
import math
import os
import re
import time
from datetime import datetime, timezone
from pathlib import Path

import pytest

LOG_ENV = "METALCLAW_PROBE_LOG"
LOG_NAME = "probe_telemetry.jsonl"
SESSION = "session"
# Eight buckets per octave of microseconds: a percentile read from them is
# within 9 % of the true value, and sessions merge by adding bucket counts.
BUCKETS_PER_OCTAVE = 8
SAMPLES = 20
MESSAGE_CHARS = 400
END_READ_TIMEOUT_S = 2.0
PROBE_COUNTERS = ("transfers", "errors", "short_replies")
PROBE_PROPERTIES = ("enabled", "clock_hz", "connected") + PROBE_COUNTERS
SNAPSHOT_PREFIX = "PROBE "

# Runs on the harness. A property the firmware does not have reads None, and
# any failure prints as a snapshot error, so the REPL call it rides on never
# raises because of it.
PROBE_SNAPSHOT = f"""import metalclaw, sys
try:
    print({SNAPSHOT_PREFIX!r} + repr({{'board': sys.implementation._build, 'probes': [
        dict([('name', i.name)] + [(p, getattr(i, p, None)) for p in {PROBE_PROPERTIES!r}])
        for i in metalclaw.instances() if i.model == 'swd_probe']}}))
except Exception as e:
    print({SNAPSHOT_PREFIX!r} + repr({{'error': repr(e)}}))
"""


def bucket(seconds: float) -> int:
    microseconds = seconds * 1e6
    if microseconds < 1.0:
        return 0
    return math.floor(math.log2(microseconds) * BUCKETS_PER_OCTAVE)


def bucket_upper_s(index: int) -> float:
    return 2.0 ** ((index + 1) / BUCKETS_PER_OCTAVE) / 1e6


class Timing:
    def __init__(self):
        self.count = 0
        self.total = 0.0
        self.max = 0.0
        self.buckets: dict[int, int] = {}

    def add(self, seconds: float) -> None:
        seconds = max(0.0, seconds)
        self.count += 1
        self.total += seconds
        if seconds > self.max:
            self.max = seconds
        index = bucket(seconds)
        self.buckets[index] = self.buckets.get(index, 0) + 1

    def merge(self, other: "Timing") -> None:
        self.count += other.count
        self.total += other.total
        self.max = max(self.max, other.max)
        for index, count in other.buckets.items():
            self.buckets[index] = self.buckets.get(index, 0) + count

    # The upper edge of the bucket holding the q-th quantile, never above
    # the largest duration seen.
    def percentile(self, q: float) -> float | None:
        if not self.count:
            return None
        rank = max(1, math.ceil(q * self.count))
        seen = 0
        for index in sorted(self.buckets):
            seen += self.buckets[index]
            if seen >= rank:
                return min(bucket_upper_s(index), self.max)
        return self.max

    def to_json(self) -> dict:
        return {"count": self.count, "total_s": self.total, "max_s": self.max,
                "buckets": {str(index): count for index, count in sorted(self.buckets.items())}}

    @classmethod
    def from_json(cls, data: dict) -> "Timing":
        timing = cls()
        timing.count = int(data.get("count", 0))
        timing.total = float(data.get("total_s", 0.0))
        timing.max = float(data.get("max_s", 0.0))
        timing.buckets = {int(index): int(count) for index, count in data.get("buckets", {}).items()}
        return timing


# One kind of call: calls == ok + sum(errors) always. A call whose transfer
# succeeded but whose data a caller then rejected moves from ok to its kind.
class Operation:
    def __init__(self, layer: str, name: str):
        self.layer = layer
        self.name = name
        self.calls = 0
        self.ok = 0
        self.errors: dict[str, int] = {}
        self.retries = 0
        self.recovered = 0
        self.words = 0
        self.timing = Timing()

    def merge(self, other: "Operation") -> None:
        self.calls += other.calls
        self.ok += other.ok
        for kind, count in other.errors.items():
            self.errors[kind] = self.errors.get(kind, 0) + count
        self.retries += other.retries
        self.recovered += other.recovered
        self.words += other.words
        self.timing.merge(other.timing)

    def to_json(self) -> dict:
        return {"layer": self.layer, "operation": self.name, "calls": self.calls, "ok": self.ok,
                "errors": dict(sorted(self.errors.items())), "retries": self.retries,
                "recovered": self.recovered, "words": self.words, "timing": self.timing.to_json()}

    @classmethod
    def from_json(cls, data: dict) -> "Operation":
        operation = cls(str(data["layer"]), str(data["operation"]))
        operation.calls = int(data.get("calls", 0))
        operation.ok = int(data.get("ok", 0))
        operation.errors = {str(kind): int(count) for kind, count in data.get("errors", {}).items()}
        operation.retries = int(data.get("retries", 0))
        operation.recovered = int(data.get("recovered", 0))
        operation.words = int(data.get("words", 0))
        operation.timing = Timing.from_json(data.get("timing", {}))
        return operation


def _iso(seconds: float | None) -> str | None:
    if seconds is None:
        return None
    return datetime.fromtimestamp(seconds, timezone.utc).isoformat(timespec="milliseconds")


class Recorder:
    def __init__(self, source: str = "rig", clock=time.perf_counter, wall=time.time):
        self.source = source
        self.clock = clock
        self._wall = wall
        self._origin = clock()
        self.started = wall()
        self.ended = None
        self.operations: dict[tuple[str, str], Operation] = {}
        self.counters: dict[str, int] = {}
        self.samples: list[dict] = []
        self.context: dict = {}
        self.probe_start = None
        self.probe_end = None
        self.probe_note = None

    def _operation(self, layer: str, name: str) -> Operation:
        key = (layer, name)
        operation = self.operations.get(key)
        if operation is None:
            operation = self.operations[key] = Operation(layer, name)
        return operation

    def _sample(self, layer, name, kind, message) -> None:
        if len(self.samples) < SAMPLES:
            self.samples.append({"at_s": round(self.clock() - self._origin, 6), "layer": layer,
                                 "operation": name, "kind": kind,
                                 "message": None if message is None else str(message)[:MESSAGE_CHARS]})

    def record(self, layer: str, name: str, seconds: float, kind: str = "ok", message=None, words: int = 0) -> None:
        operation = self._operation(layer, name)
        operation.calls += 1
        operation.timing.add(seconds)
        if kind == "ok":
            operation.ok += 1
            operation.words += words
        else:
            operation.errors[kind] = operation.errors.get(kind, 0) + 1
            self._sample(layer, name, kind, message)

    def reject(self, layer: str, name: str, kind: str, message=None) -> None:
        operation = self._operation(layer, name)
        if operation.ok:
            operation.ok -= 1
        else:
            operation.calls += 1
        operation.errors[kind] = operation.errors.get(kind, 0) + 1
        self._sample(layer, name, kind, message)

    def retry(self, layer: str, name: str, recovered: bool) -> None:
        operation = self._operation(layer, name)
        operation.retries += 1
        operation.recovered += int(bool(recovered))

    def count(self, name: str, n: int = 1) -> None:
        self.counters[name] = self.counters.get(name, 0) + n

    def describe(self, **context) -> None:
        self.context.update(context)

    def used(self) -> bool:
        return bool(self.operations) or "bench" in self.context

    def finish(self) -> None:
        if self.ended is None:
            self.ended = self._wall()

    def line(self) -> dict:
        ended = self.ended if self.ended is not None else self._wall()
        bench = dict(self.context.get("bench", {}))
        for snapshot in (self.probe_end, self.probe_start):
            if snapshot and snapshot.get("board") and not bench.get("board"):
                bench["board"] = snapshot["board"]
        line = {key: value for key, value in self.context.items() if key != "bench"}
        line.update({
            "record": SESSION,
            "source": self.source,
            "bench": bench,
            "start": _iso(self.started),
            "end": _iso(ended),
            "seconds": round(ended - self.started, 3),
            "operations": [operation.to_json() for _, operation in sorted(self.operations.items())],
            "counters": dict(sorted(self.counters.items())),
            "probe": {"start": self.probe_start, "end": self.probe_end,
                      "delta": probe_delta(self.probe_start, self.probe_end), "note": self.probe_note},
            "samples": self.samples,
        })
        return line


# Recording turned off: the same calls, kept nowhere.
class Discard:
    @staticmethod
    def clock() -> float:
        return 0.0

    def record(self, *args, **kwargs) -> None:
        pass

    def reject(self, *args, **kwargs) -> None:
        pass

    def retry(self, *args, **kwargs) -> None:
        pass

    def count(self, *args, **kwargs) -> None:
        pass

    def describe(self, **context) -> None:
        pass


DISCARD = Discard()


# The kind an access failure is counted under. pyusb's classes are named
# rather than imported, so a host without pyusb can still read the logs.
def error_kind(error: BaseException) -> str:
    kind = getattr(error, "kind", None)
    if isinstance(kind, str) and kind:
        return kind
    names = {cls.__name__ for cls in type(error).__mro__}
    if "USBTimeoutError" in names:
        return "usb_timeout"
    if "USBError" in names:
        return "usb_error"
    if isinstance(error, TimeoutError):
        return "timeout"
    return type(error).__name__


def parse_snapshot(output: str) -> tuple[dict | None, str]:
    snapshot = None
    rest = []
    for line in output.splitlines():
        if line.startswith(SNAPSHOT_PREFIX):
            try:
                parsed = ast.literal_eval(line[len(SNAPSHOT_PREFIX):])
            except (ValueError, SyntaxError) as error:
                parsed = {"error": f"unreadable snapshot: {error}"}
            snapshot = parsed if isinstance(parsed, dict) else {"error": f"unreadable snapshot: {parsed!r}"}
        else:
            rest.append(line)
    return snapshot, "\n".join(rest)


def _first_probe(snapshot: dict | None) -> dict | None:
    if not snapshot or "probes" not in snapshot:
        return None
    probes = snapshot["probes"]
    return probes[0] if probes else None


# What the probe counted during the session. A probe that was not attached at
# the start was attached by the session, so it counted from zero; counters
# that went down were reset part way, and the session's share is unknown.
def probe_delta(start: dict | None, end: dict | None) -> dict | None:
    last = _first_probe(end)
    if last is None:
        return None
    if start is not None and "error" in start:
        return None
    first = _first_probe(start)
    delta = {}
    for name in PROBE_COUNTERS:
        after = last.get(name)
        if after is None:
            continue
        before = 0 if first is None else first.get(name)
        if before is None or after < before:
            return {"reset": True}
        delta[name] = after - before
    return delta


def describe_ops(recorder: Recorder, layer: str, noun: str) -> str | None:
    operations = [operation for (op_layer, _), operation in recorder.operations.items() if op_layer == layer]
    if not operations:
        return None
    calls = sum(operation.calls for operation in operations)
    kinds: dict[str, int] = {}
    for operation in operations:
        for kind, count in operation.errors.items():
            kinds[kind] = kinds.get(kind, 0) + count
    failed = sum(kinds.values())
    text = f"{calls} {noun}, {failed} failed"
    if kinds:
        text += " (" + ", ".join(f"{kind} {count}" for kind, count in sorted(kinds.items())) + ")"
    retries = sum(operation.retries for operation in operations)
    if retries:
        text += f", {retries} retried"
    return text


def summary(recorder: Recorder) -> str:
    parts = [text for text in (describe_ops(recorder, "probe_rs", "probe-rs runs"),
                               describe_ops(recorder, "dap", "DAP accesses")) if text]
    delta = probe_delta(recorder.probe_start, recorder.probe_end)
    if delta and not delta.get("reset"):
        parts.append("probe counted " + ", ".join(f"{name} +{value}" for name, value in delta.items()))
    elif delta:
        parts.append("probe counters were reset during the session")
    elif recorder.probe_note:
        parts.append(f"probe counters: {recorder.probe_note}")
    return "; ".join(parts) or "no probe traffic"


def read_probe(run, timeout: float = END_READ_TIMEOUT_S) -> tuple[dict | None, str | None]:
    try:
        snapshot, _ = parse_snapshot(run(PROBE_SNAPSHOT, timeout))
    except Exception as error:
        return None, f"not read: {error!r}"[:MESSAGE_CHARS]
    if snapshot is None:
        return None, "the harness printed no snapshot"
    if "error" in snapshot:
        return snapshot, f"the harness could not read its probe: {snapshot['error']}"
    if not snapshot.get("probes"):
        return snapshot, "no swd_probe attached"
    return snapshot, None


def log_path(config) -> Path | None:
    named = os.environ.get(LOG_ENV, "").strip()
    if named:
        return Path(named).expanduser()
    junit = getattr(config.option, "xmlpath", None)
    if junit:
        return Path(junit).expanduser().absolute().parent / LOG_NAME
    return None


def append(path: Path, line: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as log:
        log.write(json.dumps(line, sort_keys=True) + "\n")


RECORDER = pytest.StashKey[Recorder]()
WRITTEN = pytest.StashKey[Path]()


def recorder(config) -> Recorder | None:
    return config.stash.get(RECORDER, None)


# For a platform's probe fixture, after the last case that uses the probe.
def read_end(config, harness) -> None:
    session = recorder(config)
    if session is None:
        return
    session.probe_end, session.probe_note = read_probe(harness.run)


def pytest_configure(config) -> None:
    session = Recorder("rig")
    platform = os.environ.get("METALCLAW_PLATFORM")
    if platform:
        session.describe(platform=platform)
    config.stash[RECORDER] = session


# After the runner's own, which tears down the fixtures and with them the
# probe fixture's last read.
@pytest.hookimpl(trylast=True)
def pytest_sessionfinish(session, exitstatus) -> None:
    recorded = recorder(session.config)
    if recorded is None or not recorded.used():
        return
    recorded.finish()
    if recorded.probe_end is None and recorded.probe_note is None:
        recorded.probe_note = "not read: the session never set up the probe"
    recorded.describe(exitstatus=int(exitstatus))
    path = log_path(session.config)
    if path is None:
        return
    try:
        append(path, recorded.line())
    except OSError as error:
        recorded.probe_note = f"{recorded.probe_note or ''}; log not written: {error}".lstrip("; ")
        return
    session.config.stash[WRITTEN] = path


def pytest_terminal_summary(terminalreporter, config) -> None:
    recorded = recorder(config)
    if recorded is None or not recorded.used():
        return
    written = config.stash.get(WRITTEN, None)
    where = f"; logged to {written}" if written else f"; not logged (set {LOG_ENV} or --junitxml)"
    terminalreporter.write_line("probe: " + summary(recorded) + where)


# probe-rs reports every failure as text; these are the kinds that text is
# counted under, first match wins. The text is kept in the samples.
PROBE_RS_KINDS = (
    (re.compile(r"failed to open probe", re.I), "probe_open"),
    (re.compile(r"error in the usb access|\busb\b", re.I), "usb"),
    (re.compile(r"error handling cmsis-dap command", re.I), "dap_command"),
    (re.compile(r"verif", re.I), "verify"),
    (re.compile(r"\bfault\b", re.I), "fault"),
    (re.compile(r"\bwait\b", re.I), "wait"),
    (re.compile(r"no ?ack", re.I), "no_ack"),
    (re.compile(r"timed? ?out", re.I), "probe_timeout"),
)


def probe_rs_kind(output: str) -> str:
    for pattern, kind in PROBE_RS_KINDS:
        if pattern.search(output):
            return kind
    return "exit"
