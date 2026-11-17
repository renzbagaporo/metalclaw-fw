# SPDX-License-Identifier: Apache-2.0 OR MIT
# The upstream Zephyr samples the rig builds and judges, and how each is
# built: unmodified from the pinned Zephyr tree, with the sample.yaml (or
# testcase.yaml) test's own extra_args and extra_configs, plus only what a
# Metalclaw bench needs, as overlays and Kconfig fragments added on top
# (EXTRA_DTC_OVERLAY_FILE, EXTRA_CONF_FILE), never in place of the sample's
# own board files. test/zephyr_samples/build.py builds them.

from dataclasses import dataclass

# The Zephyr board each bench's DUT is, by the board name example.json uses
# for it (as pico-examples builds name theirs), and its chip.
BOARDS = {
    "pico": ("rpi_pico", "rp2040"),
    "pico2": ("rpi_pico2/rp2350a/m33", "rp2350"),
}


@dataclass(frozen=True)
class Build:
    # The sample's directory under samples/.
    sample: str
    # The yaml test to build, or None for its first test that runs on a
    # console (or ztest) harness and is not build_only.
    test: str | None = None
    # Extra CMake cache entries ("NAME=value"), after the test's own.
    args: tuple[str, ...] = ()
    # The boards (BOARDS keys) it builds for, and why the others are left out.
    boards: tuple[str, ...] = ("pico", "pico2")
    other_boards: str = ""


def _console(*samples: str) -> dict[str, Build]:
    return {f"zephyr/{s}": Build(s) for s in samples}


BUILDS: dict[str, Build] = {
    # Wave 1a: console-only samples, judged by their own sample.yaml.
    **_console(
        "hello_world",
        "basic/hash_map",
        "basic/sys_heap",
        "synchronization",
        "kernel/condition_variables/condvar",
        "kernel/condition_variables/simple",
        "kernel/metairq_dispatch",
        "kernel/msg_queue",
        "cpp/hello_world",
        "cpp/cpp_synchronization",
        "data_structures/min-heap",
        "data_structures/rbtree",
        "application_development/external_lib",
        "posix/env",
        "posix/eventfd",
        "posix/philosophers",
        "posix/uname",
        "subsys/portability/cmsis_rtos_v1/philosophers",
        "subsys/portability/cmsis_rtos_v1/timer_synchronization",
        "subsys/portability/cmsis_rtos_v2/philosophers",
        "subsys/portability/cmsis_rtos_v2/timer_synchronization",
        "subsys/bindesc/hello_bindesc",
        "subsys/bindesc/read_bindesc",
        "subsys/crc",
        "subsys/llext/modules",
        "subsys/logging/logger",
        "subsys/pm/device_pm",
        "subsys/rtio/producer_consumer",
        "subsys/rtio/sensor_batch_processing",
        "subsys/smf/hsm_psicc2",
        "subsys/task_wdt",
        "subsys/tracing",
        "subsys/testsuite/integration",
        "subsys/testsuite/benchmark",
        "subsys/zbus/async_listeners",
        "subsys/zbus/benchmark",
        "subsys/zbus/confirmed_channel",
        "subsys/zbus/dyn_channel",
        "subsys/zbus/hello_world",
        "subsys/zbus/msg_subscriber",
        "subsys/zbus/priority_boost",
        "subsys/zbus/runtime_obs_registration",
        "subsys/zbus/work_queue",
        "sensor/die_temp_polling",
        "drivers/counter/alarm",
        "drivers/watchdog",
        "drivers/led/pwm",
        "drivers/adc/adc_dt",
    ),
    # samples/kernel/bootargs: the tokenization test is the one that runs
    # outside QEMU (its extra_configs set the boot arguments).
    "zephyr/kernel/bootargs": Build("kernel/bootargs", test="sample.kernel.bootargs.tokenization"),
    # samples/philosophers prints its states only with DEBUG_PRINTF=1, as
    # its first test sets in extra_args.
    "zephyr/philosophers": Build("philosophers", test="sample.kernel.philosopher"),
    # samples/sensor/clock: its system-clock test, which twister builds only
    # where CONFIG_SENSOR_CLOCK_SYSTEM is on; the RP2 boards leave it off,
    # and the counter and RTC tests are for nRF52 boards.
    "zephyr/sensor/clock": Build("sensor/clock", test="sample.sensor.clock.system",
                                 args=("CONFIG_SENSOR_CLOCK_SYSTEM=y",)),
    # samples/basic/blinky: its one test, on an LED harness (judged by our
    # own criterion, test/zephyr_samples/targets.py OWN).
    "zephyr/basic/blinky": Build("basic/blinky", test="sample.basic.blinky"),
    # samples/net/sockets/socketpair: its first test, on a "net" harness with
    # no patterns (judged by our own criterion).
    "zephyr/net/sockets/socketpair": Build("net/sockets/socketpair", test="sample.net.sockets.socketpair"),
    # The Pico 1 builds take the samples' own boards/rpi_pico.overlay (PWM on
    # the LED, the on-chip RTC); the Pico 2 builds need an overlay of ours.
    **{f"zephyr/{s}": Build(s, boards=("pico",), other_boards="the sample has no rpi_pico2 overlay yet")
       for s in ("basic/blinky_pwm", "basic/fade_led", "drivers/rtc")},
}
