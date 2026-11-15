# SPDX-License-Identifier: Apache-2.0 OR MIT
# What "completed" means for each pico-examples program, from its own source:
# the lines it prints on the stdio UART, in order, and what it must never
# print. A program runs unmodified, built for the DUT's board from the pinned
# pico-examples tree (example.json beside the ELF names it).

import re
from dataclasses import dataclass, field, replace

# The SDK's panic() and assert() paths, and a fault the example would print.
FORBIDDEN = (r"\*\*\* PANIC \*\*\*", r"assertion \".*\" failed", r"HardFault")


@dataclass(frozen=True)
class Spec:
    # Regular expressions, each matched after the previous one's match.
    console: tuple[str, ...]
    # Bench seconds the program gets to print them all.
    seconds: float = 5.0
    forbidden: tuple[str, ...] = FORBIDDEN
    # The source the criteria were read from, relative to pico-examples.
    source: str = ""
    notes: str = ""
    extra: dict = field(default_factory=dict)
    # The boards (example.json "board") whose build this Spec can judge;
    # empty for every board it builds for.
    boards: tuple[str, ...] = ()
    # Fields that differ in one board's build, by board: {"pico": {...}}.
    per_board: dict = field(default_factory=dict)

    def for_board(self, board: str | None) -> "Spec | None":
        # The criteria for this board's build, or None where they cannot judge it.
        if self.boards and board not in self.boards:
            return None
        return replace(self, boards=(), per_board={}, **self.per_board.get(board, {}))

    def match(self, text: str) -> tuple[list[str], list[str]]:
        # The expected patterns not found in order, and the forbidden ones found.
        missing, at = [], 0
        for pattern in self.console:
            m = re.compile(pattern).search(text, at)
            if m is None:
                missing.append(pattern)
            else:
                at = m.end()
        found = [p for p in self.forbidden if re.search(p, text)]
        return missing, found


SPECS: dict[str, Spec] = {
    # hello_world/serial/hello_serial.c: stdio_init_all(), then
    # printf("Hello, world!\n") every second, forever.
    "hello_serial": Spec(
        console=(r"Hello, world!\r?\n",) * 3,
        seconds=4.0,
        source="hello_world/serial/hello_serial.c",
    ),
}
