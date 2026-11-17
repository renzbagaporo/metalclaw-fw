# SPDX-License-Identifier: Apache-2.0 OR MIT
# What "completed" means for an upstream Zephyr sample: the console patterns
# of its own sample.yaml test, matched as twister matches them, after the
# boot banner, with Zephyr's fault reports forbidden. A sample builds
# unmodified from the pinned Zephyr tree (test/zephyr_samples/builds.py), and
# the build copies the test's harness_config into the example.json beside
# its zephyr.elf, so the criteria are the ones the image was built against.
# OVERRIDES holds what a sample needs beyond its yaml, with the reason.
# The pico-examples cases (test/pico_examples/cases.py) judge it with these.

from dataclasses import replace

from test.pico_examples.targets import Spec, every

# The boot banner every sample prints first (kernel/banner.c, with
# CONFIG_BOOT_BANNER on and BOOT_BANNER_STRING "Booting Zephyr OS build" by
# default, kernel/Kconfig), as one console line.
BANNER = r"^\*\*\* Booting Zephyr OS build .* \*\*\*$"

# Zephyr's fatal error and assertion reports (kernel/fatal.c, sys/__assert.h),
# and a second boot banner: a fault prints no report with CONFIG_LOG off, but
# the reset after it shows as a second banner. Matched on the whole console.
FORBIDDEN = (
    r">>> ZEPHYR FATAL ERROR",
    r"ASSERTION FAIL",
    r"\*\*\* Booting Zephyr OS[^\n]*\n[\s\S]*\*\*\* Booting Zephyr OS",
)

# For a sample that resets the chip on purpose, whose yaml expects it to
# boot again: the fault reports only.
FAULTS = FORBIDDEN[:2]

# Bench seconds a sample gets to print its patterns, unless it overrides.
SECONDS = 10.0

# By target: Spec fields that replace the ones the yaml gives.
OVERRIDES: dict[str, dict] = {
    # samples/drivers/led/pwm/src/main.c:80-140: on, off, a fade up and down
    # (CONFIG_FADE_DELAY steps), then blinking, with one-second pauses: on
    # the carrier the blinking starts 7 s in, so the loop ends after 10 s.
    "zephyr/drivers/led/pwm": {"seconds": 30.0},
    # samples/drivers/watchdog: feeds the watchdog five times, then lets it
    # reset the chip; its yaml expects the second boot.
    "zephyr/drivers/watchdog": {"forbidden": FAULTS, "seconds": 20.0},
    # samples/subsys/task_wdt: the control thread gets stuck and the task
    # watchdog resets the chip.
    "zephyr/subsys/task_wdt": {"forbidden": FAULTS, "seconds": 20.0},
}


def own(*console: str, source: str, **fields) -> Spec:
    fields = {"seconds": SECONDS, "forbidden": FORBIDDEN, **fields}
    return Spec(console=(BANNER, *console), lines=True, source=source, **fields)


# By target: criteria of ours, for a sample whose yaml judges it some other
# way than its console (an LED harness, a host script), read from its source.
OWN: dict[str, Spec] = {
    # samples/basic/blinky/src/main.c:37-46: toggles led0 and prints its new
    # state every SLEEP_TIME_MS = 1000 ms, starting from ON (so OFF first).
    # Its yaml's harness is "led", which the bench does not have.
    "zephyr/basic/blinky": own(r"^LED state: OFF$", r"^LED state: ON$", r"^LED state: OFF$",
                               intervals=every(1.0, 0.005, 3, first=1),
                               source="samples/basic/blinky/src/main.c"),
    # samples/subsys/bindesc/read_bindesc/src/main.c:53-126: finds the
    # descriptors in flash, then reads the kernel version through mapped
    # flash, RAM and the flash driver, and its own my_bytes {1, 2, 3, 4};
    # the search for id 123 is meant to miss. Its yaml is build_only.
    "zephyr/subsys/bindesc/read_bindesc": own(
        r"^Found descriptors at 0x[0-9a-f]+$", r"^Reading using memory mapped flash:$",
        r"^Zephyr version: \d+\.\d+\.\d+", r"^Reading using RAM:$", r"^Descriptor not found!$",
        r"^Zephyr version number: \d+$", r"^Reading using flash:$", r"^my_bytes: 01 02 03 04 ?$",
        forbidden=(*FORBIDDEN, r"Descriptors not found"),
        source="samples/subsys/bindesc/read_bindesc/src/main.c"),
    # samples/net/sockets/socketpair/src/main.c:230-257: three threads write
    # their names through socketpairs that main polls; it prints SUCCESS or
    # FAILURE after tearing down, and a short or wrong read on the way. Its
    # yaml's harness is "net", with no patterns.
    "zephyr/net/sockets/socketpair": own(
        r"^setting-up$", r"^tearing-down$", r"^SUCCESS$",
        forbidden=(*FORBIDDEN, r"only wrote", r"only read", r"expected", r"FAILURE", r"error",
                   r"unhandled events remaining", r"failed to find fd"),
        source="samples/net/sockets/socketpair/src/main.c"),
}


def banner(metadata: dict) -> tuple[str, ...]:
    # The banner pattern, for an image built with the banner on (a sample may
    # turn CONFIG_BOOT_BANNER off: basic/hash_map).
    return (BANNER,) if metadata.get("boot_banner", True) else ()


def is_zephyr(metadata: dict | None) -> bool:
    return bool(metadata) and metadata.get("kind") == "zephyr"


def zephyr_spec(metadata: dict | None) -> Spec | None:
    # The Spec for a Zephyr sample's build, or None for another program or a
    # test whose harness is not the console.
    if not is_zephyr(metadata):
        return None
    if metadata["target"] in OWN:
        return OWN[metadata["target"]]
    source = f"samples/{metadata['sample']}/sample.yaml ({metadata['test']})"
    if metadata.get("harness") == "ztest":
        # A ztest image's summary line (TC_END_REPORT,
        # subsys/testsuite/include/zephyr/tc_util.h), which twister's Test
        # harness reads.
        spec = Spec(console=(*banner(metadata), r"^PROJECT EXECUTION SUCCESSFUL$"), seconds=SECONDS,
                    forbidden=(*FORBIDDEN, r"PROJECT EXECUTION FAILED"), lines=True, source=source)
        return replace(spec, **OVERRIDES.get(metadata["target"], {}))
    if metadata.get("harness") != "console":
        return None
    config = metadata["harness_config"]
    regex = tuple(config["regex"])
    if config["type"] == "one_line":
        patterns, ordered = regex[:1], True
    else:
        patterns, ordered = regex, config.get("ordered", True)
    spec = Spec(console=(*banner(metadata), *patterns), seconds=SECONDS, forbidden=FORBIDDEN, lines=True,
                ordered=ordered, source=source)
    return replace(spec, **OVERRIDES.get(metadata["target"], {}))
