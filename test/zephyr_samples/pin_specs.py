# SPDX-License-Identifier: Apache-2.0 OR MIT
# What an upstream Zephyr sample does on its pins, watched over SWD as a
# pico-examples program's are (test/pico_examples/pin_specs.py, pins.py),
# from each sample's source. Zephyr switches the pad input off on a pin it
# only drives; the watch then reads the pin's output (pins.watch).

from test.pico_examples.pin_specs import LED, PinSpec, half, toggles
from test.pico_examples.pins import Toggle

ZEPHYR_PIN_SPECS: dict[str, PinSpec] = {
    # samples/basic/blinky/src/main.c:37-46: led0 (GP25 on the Picos)
    # toggles every SLEEP_TIME_MS, 1000 ms.
    "zephyr/basic/blinky": PinSpec(4.0, (Toggle(LED, toggles(4.0, 1.0), half(1.0)),), "samples/basic/blinky/src/main.c"),
    # samples/basic/threads/src/main.c:54-97: led0 every 100 ms, led1 (GP14,
    # test/zephyr_samples/overlays/threads_led1.overlay) every 1000 ms.
    "zephyr/basic/threads": PinSpec(3.0, (Toggle(LED, toggles(3.0, 0.1), half(0.1)),
                                          Toggle(14, toggles(3.0, 1.0), half(1.0))),
                                    "samples/basic/threads/src/main.c"),
}
