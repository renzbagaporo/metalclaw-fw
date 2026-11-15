# SPDX-License-Identifier: Apache-2.0 OR MIT
# Pin behaviour that shows a pico-examples program completed, for programs
# judged by their pins rather than (or as well as) their console: the pins
# each one drives, watched over SWD (pins.py). Criteria come from each
# program's source; times are the program's own delays with a margin for
# one poll.

from dataclasses import dataclass

from test.pico_examples.pins import Toggle


@dataclass(frozen=True)
class Press:
    # The board's BOOTSEL button held from `at` to `release`, seconds into
    # the watch.
    at: float
    release: float


@dataclass(frozen=True)
class PinSpec:
    seconds: float
    # The checks: Toggle, Morse, Segments, Levels (pins.py).
    toggles: tuple
    source: str = ""
    # What the case does while it watches: (seconds, bytes) typed on the
    # console, and presses of the BOOTSEL button.
    send: tuple[tuple[float, bytes], ...] = ()
    presses: tuple[Press, ...] = ()


@dataclass(frozen=True)
class BootselSpec:
    # Resets on RUN: held low `low_s`, `gap_s` between them. After one, the
    # program blinks `led` at least `edges` times in `seconds`; after two
    # the bootrom presents USB and the LED stops.
    led: int
    low_s: float
    gap_s: float
    seconds: float
    edges: int
    source: str = ""


def half(seconds: float) -> tuple[float, float]:
    # A half period with room for one poll either side and timer jitter.
    return (seconds * 0.92 - 0.004, seconds * 1.08 + 0.004)


# PICO_DEFAULT_LED_PIN is GP25 on both pico and pico2 (pico-sdk
# src/boards/include/boards/pico.h, pico2.h).
LED = 25

PIN_SPECS: dict[str, PinSpec] = {
    # blink/blink.c:46-51: LED on, sleep_ms(LED_DELAY_MS), off, sleep, forever;
    # LED_DELAY_MS 250 (blink.c:16).
    "blink": PinSpec(3.0, (Toggle(LED, 8, (0.24, 0.27)),), "blink/blink.c"),
    # blink_simple/blink_simple.c:36-41: the same loop, LED_DELAY_MS 250 (:10).
    "blink_simple": PinSpec(3.0, (Toggle(LED, 8, (0.24, 0.27)),), "blink_simple/blink_simple.c"),
    # binary_info/blink_any/blink_any.c:32-34, 70-75: LED_PIN (bi_ptr, 25 by
    # default) toggles every 250 ms.
    "blink_any": PinSpec(3.0, (Toggle(LED, 8, half(0.25)),), "binary_info/blink_any/blink_any.c"),
    # async_context/simple_at_time_worker/simple_at_time_worker.c:22-33, 66: an at-time worker
    # toggles the LED every 500 ms.
    "simple_at_time_worker": PinSpec(4.0, (Toggle(LED, 5, half(0.5)),),
                                     "async_context/simple_at_time_worker/simple_at_time_worker.c"),
    # status_led/status_blink/status_blink.c:10-24: 250 ms half periods.
    "status_blink": PinSpec(3.0, (Toggle(LED, 8, half(0.25)),), "status_led/status_blink/status_blink.c"),
    # system/hello_double_tap/hello_double_tap.c:20-25: 250 ms half periods.
    "hello_double_tap": PinSpec(3.0, (Toggle(LED, 8, half(0.25)),), "system/hello_double_tap/hello_double_tap.c"),
    # usb/device/dev_hid_composite: TinyUSB's board LED blinks every 250 ms
    # while unmounted; the DUT's USB reaches no host on either bench.
    "dev_hid_composite": PinSpec(3.0, (Toggle(LED, 8, half(0.25)),), "usb/device/dev_hid_composite/main.c"),
    # bootloaders/uart/uart_binary.c:12-14, 79-85 (pico2, no_flash): 500 ms half periods.
    "uart_binary": PinSpec(4.0, (Toggle(LED, 5, half(0.5)),), "bootloaders/uart/uart_binary.c"),
    # low_power/low_power_dormant/low_power_clksrc.c:17-21: the LED toggles every 500 ms.
    "low_power_clksrc": PinSpec(4.0, (Toggle(LED, 5, half(0.5)),), "low_power/low_power_dormant/low_power_clksrc.c"),
    # pio/pio_blink/blink.c:30-57, 118-142: PIO square waves for 60 s, GP3 at
    # 4 Hz, GP4 3 Hz, GP5 2 Hz, GP6 1 Hz. The watcher reads the DUT's own
    # pins, so GP3 counts on fixture 1 too.
    "pio_blink": PinSpec(4.0, (Toggle(3, 20, half(1 / 8)), Toggle(4, 16, half(1 / 6)),
                               Toggle(5, 10, half(1 / 4)), Toggle(6, 5, half(1 / 2))), "pio/pio_blink/blink.c"),
}

BOOTSEL_SPECS: dict[str, BootselSpec] = {}
