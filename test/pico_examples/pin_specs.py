# SPDX-License-Identifier: Apache-2.0 OR MIT
# Pin behaviour that shows a pico-examples program completed, for programs
# judged by their pins rather than (or as well as) their console: the pins
# each one drives, watched over SWD (pins.py). Criteria come from each
# program's source; times are the program's own delays with a margin for
# one poll.

from dataclasses import dataclass

from test.pico_examples.pins import Toggle


@dataclass(frozen=True)
class PinSpec:
    seconds: float
    toggles: tuple[Toggle, ...]
    source: str = ""


PIN_SPECS: dict[str, PinSpec] = {}
