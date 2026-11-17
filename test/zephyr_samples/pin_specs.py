# SPDX-License-Identifier: Apache-2.0 OR MIT
# What an upstream Zephyr sample does on its pins, watched over SWD as a
# pico-examples program's are (test/pico_examples/pin_specs.py, pins.py),
# from each sample's source. Zephyr switches the pad input off on a pin it
# only drives; the watch then reads the pin's output (pins.watch).

from test.pico_examples.pin_specs import LED, PinSpec, half, toggles
from test.pico_examples.pins import Count, Toggle

# drivers/auxdisplay/auxdisplay_gpio_7seg.c:16-27: the driver's patterns
# for 0..9, segment A in bit 0 (its 9 lights D, unlike pins.SEGMENT_DIGITS).
GPIO_7SEG_DIGITS = (0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F)

ZEPHYR_PIN_SPECS: dict[str, PinSpec] = {
    # samples/basic/blinky/src/main.c:37-46: led0 (GP25 on the Picos)
    # toggles every SLEEP_TIME_MS, 1000 ms.
    "zephyr/basic/blinky": PinSpec(4.0, (Toggle(LED, toggles(4.0, 1.0), half(1.0)),), "samples/basic/blinky/src/main.c"),
    # samples/basic/threads/src/main.c:54-97: led0 every 100 ms, led1 (GP14,
    # test/zephyr_samples/overlays/threads_led1.overlay) every 1000 ms.
    "zephyr/basic/threads": PinSpec(3.0, (Toggle(LED, toggles(3.0, 0.1), half(0.1)),
                                          Toggle(14, toggles(3.0, 1.0), half(1.0))),
                                    "samples/basic/threads/src/main.c"),
    # samples/drivers/auxdisplay_digits/src/main.c:24-120: after about 3 s
    # of all-segments, blank and "0", it counts up from 0 a digit every
    # 100 ms, 9 wrapping to 0, forever (one digit, overlays/
    # auxdisplay_digits_gp2_10.overlay). 6 s of watch holds at least 3 s
    # of the count: 30 changes, 20 with room for the watch's start.
    "zephyr/drivers/auxdisplay_digits": PinSpec(6.0, (Count(2, 20, GPIO_7SEG_DIGITS),),
                                                "samples/drivers/auxdisplay_digits/src/main.c"),
}
