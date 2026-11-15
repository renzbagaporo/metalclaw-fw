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


# PICO_DEFAULT_LED_PIN is GP25 on both pico and pico2 (pico-sdk
# src/boards/include/boards/pico.h, pico2.h).
LED = 25

PIN_SPECS: dict[str, PinSpec] = {
    # blink/blink.c:46-51: LED on, sleep_ms(LED_DELAY_MS), off, sleep, forever;
    # LED_DELAY_MS 250 (blink.c:16).
    "blink": PinSpec(3.0, (Toggle(LED, 8, (0.24, 0.27)),), "blink/blink.c"),
    # blink_simple/blink_simple.c:36-41: the same loop, LED_DELAY_MS 250 (:10).
    "blink_simple": PinSpec(3.0, (Toggle(LED, 8, (0.24, 0.27)),), "blink_simple/blink_simple.c"),
}
