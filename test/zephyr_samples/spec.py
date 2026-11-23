# SPDX-License-Identifier: Apache-2.0 OR MIT
# What "completed" means for a program on the DUT: the lines it prints on its
# console, in order, and what it must never print (Spec). A Zephyr sample's
# Spec comes from its own sample.yaml test (targets.py).

import re
from dataclasses import dataclass, field, replace

# A panic, a failed assertion and a fault, as a program would print them.
FORBIDDEN = (r"\*\*\* PANIC \*\*\*", r"assertion \".*\" failed", r"HardFault")


# VT100 escape sequences (the Zephyr shell's colours and cursor moves), as
# twister's harnesses strip them from a line.
ANSI_ESCAPE = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")


def console_lines(text: str, start: int = 0) -> list[tuple[int, str]]:
    # Each line of `text` from `start`, the last one even unfinished: where it
    # ends in `text`, and its content without the line end or VT100 sequences.
    return [(start + m.end(), ANSI_ESCAPE.sub("", m.group(0)).rstrip("\r\n"))
            for m in re.finditer(r"[^\n]*\n|[^\n]+$", text[start:])]


@dataclass(frozen=True)


class Spec:
    # Regular expressions, each matched after the previous one's match.
    console: tuple[str, ...]
    # Bench seconds the program gets to print them all.
    seconds: float = 5.0
    forbidden: tuple[str, ...] = FORBIDDEN
    # The source the criteria were read from.
    source: str = ""
    notes: str = ""
    extra: dict = field(default_factory=dict)
    # Why the program fails these criteria on a bench by design (a part
    # modelled to its datasheet, where the program expects otherwise): the
    # case expects the failure, strictly, so a pass is reported too.
    known_failure: str = ""
    # The boards (example.json "board") whose build this Spec can judge;
    # empty for every board it builds for. `other_boards` says why the
    # builds it leaves out cannot be judged.
    boards: tuple[str, ...] = ()
    other_boards: str = ""
    # Fields that differ in one board's build, by board: {"pico": {...}}.
    per_board: dict = field(default_factory=dict)
    # Fields that differ on a bench whose lines have pull-ups (the carrier's
    # 5.1 k on every Pico GP0-22), where an undriven line reads high.
    with_line_pullups: dict = field(default_factory=dict)
    # What the case does as the program runs, in order, each once `pattern`
    # has matched after where the previous one did: (pattern, bytes) types
    # on the console, (pattern, str) runs code on the harness, for a part
    # that is to act then.
    input: tuple[tuple[str, bytes | str], ...] = ()
    # The DUT pins these criteria take an analog level on, and the fields
    # that differ on a bench whose harness can only drive them high or low
    # (fixture "analog_pins").
    analog_pins: tuple[int, ...] = ()
    without_analog: dict = field(default_factory=dict)
    # (i, j, least, most): bench seconds the program takes from printing
    # what console pattern i matched to printing what pattern j matched,
    # for a program whose source prints them a set time apart (every()).
    intervals: tuple[tuple[int, int, float, float], ...] = ()
    # Each pattern matched against one console line at a time, its line end
    # and VT100 sequences stripped, as Zephyr's twister harnesses do (`^` and
    # `$` anchor to that line), and each on a later line than the one before.
    # `input` patterns then match the same way.
    lines: bool = False
    # With `lines`: each pattern matched on any line, in any order (twister's
    # multi_line with `ordered: false`).
    ordered: bool = True
    # Where the run's own output starts, when it can tell (a boot banner):
    # `forbidden` is matched from the first match of this on, since the
    # console read starts while the program from before the restart may
    # still be printing. Matched on the whole console when it has none.
    forbidden_from: str = ""

    def for_board(self, board: str | None) -> "Spec | None":
        # The criteria for this board's build, or None where they cannot judge it.
        if self.boards and board not in self.boards:
            return None
        return replace(self, boards=(), other_boards="", per_board={}, **self.per_board.get(board, {}))

    # Why `board`'s build is not judged, for the skip.
    def why_not(self, target: str, board: str | None) -> str:
        return f"{target}'s console cannot judge its {board} build" + (
            f": {self.other_boards}" if self.other_boards else "")

    def for_bench(self, line_pullups: bool, analog_pins=()) -> "Spec":
        spec = self
        if line_pullups and spec.with_line_pullups:
            spec = replace(spec, with_line_pullups={}, **spec.with_line_pullups)
        if not set(spec.analog_pins) <= set(analog_pins) and spec.without_analog:
            spec = replace(spec, without_analog={}, **spec.without_analog)
        return spec

    def match(self, text: str, patterns: tuple[str, ...] | None = None) -> tuple[list[str], list[str]]:
        # The expected patterns (`console` unless given) not found in order,
        # and the forbidden ones found.
        patterns = self.console if patterns is None else patterns
        missing = [p for p, end in zip(patterns, self.match_ends(text, patterns)) if end is None]
        start = re.search(self.forbidden_from, text) if self.forbidden_from else None
        judged = text[start.start():] if start is not None else text
        found = [p for p in self.forbidden if re.search(p, judged)]
        return missing, found

    def match_ends(self, text: str, patterns: tuple[str, ...] | None = None) -> list[int | None]:
        # Where each pattern's match ends in `text`, None where it has none.
        patterns = self.console if patterns is None else patterns
        if self.lines:
            return self._line_ends(text, patterns)
        ends, at = [], 0
        for pattern in patterns:
            m = re.compile(pattern).search(text, at)
            ends.append(None if m is None else m.end())
            if m is not None:
                at = m.end()
        return ends

    def _line_ends(self, text: str, patterns: tuple[str, ...]) -> list[int | None]:
        # Per line: where the line a pattern matched ends in `text`.
        spans = console_lines(text)
        ends, after = [], 0
        for pattern in patterns:
            rx = re.compile(pattern)
            first = after if self.ordered else 0
            hit = next((i for i in range(first, len(spans)) if rx.search(spans[i][1])), None)
            ends.append(None if hit is None else spans[hit][0])
            if hit is not None:
                after = hit + 1
        return ends

    def timing(self, text: str, arrivals) -> list[str]:
        # The intervals that every length `arrivals` allows puts outside
        # their bounds. `arrivals` holds (end, earliest, latest): the text
        # up to `end` had come in by `latest` bench seconds, and what came
        # after the previous entry not before `earliest`.
        def window(end):
            return next(((lo, hi) for e, lo, hi in arrivals if e >= end), None)
        problems, ends = [], self.match_ends(text)
        for i, j, least, most in self.intervals:
            if ends[i] is None or ends[j] is None or window(ends[j]) is None:
                continue
            (lo_i, hi_i), (lo_j, hi_j) = window(ends[i]), window(ends[j])
            if hi_j - lo_i < least or lo_j - hi_i > most:
                problems.append(f"patterns {i} to {j} came {lo_j - hi_i:.2f}..{hi_j - lo_i:.2f} s apart, "
                                f"not {least}..{most} s")
        return problems


# Consecutive patterns first..first + count - 1 printed `period` to
# `period` + `slack` seconds apart.


def every(period: float, slack: float, count: int, first: int = 0) -> tuple[tuple[int, int, float, float], ...]:
    return tuple((k, k + 1, period, period + slack) for k in range(first, first + count - 1))


NL = r"\r?\n"


def forbid(*patterns: str) -> tuple[str, ...]:
    return FORBIDDEN + patterns
