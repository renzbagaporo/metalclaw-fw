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
    # console, ztest or shell harness and is not build_only.
    test: str | None = None
    # Extra CMake cache entries ("NAME=value"), after the test's own.
    args: tuple[str, ...] = ()
    # Devicetree overlays and Kconfig fragments of ours, repository-relative,
    # added after the test's own (EXTRA_DTC_OVERLAY_FILE, EXTRA_CONF_FILE):
    # the parts' nodes and the aliases the sample looks them up by.
    overlays: tuple[str, ...] = ()
    confs: tuple[str, ...] = ()
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
        "subsys/crc",
        "subsys/logging/logger",
        "subsys/pm/device_pm",
        "subsys/rtio/producer_consumer",
        "subsys/rtio/sensor_batch_processing",
        "subsys/smf/hsm_psicc2",
        "subsys/task_wdt",
        "subsys/tracing",
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
    # samples/subsys/bindesc/read_bindesc: build_only upstream (judged by our
    # own criterion).
    "zephyr/subsys/bindesc/read_bindesc": Build("subsys/bindesc/read_bindesc", test="sample.bindesc.read_bindesc"),
    # samples/subsys/llext/modules: the module build, which loads the hello
    # world extension at run time; its platform_allow lists QEMU and MPS2
    # boards only.
    "zephyr/subsys/llext/modules": Build("subsys/llext/modules", test="sample.llext.modules.module_build"),
    # Wave 1b: typed console. The shell samples' own commands
    # (test_shell.yml) go in at the prompt; the keyboard samples are judged
    # by our own criteria.
    **_console("subsys/shell/shell_module", "drivers/flash_shell"),
    "zephyr/subsys/testsuite/pytest/shell": Build("subsys/testsuite/pytest/shell", test="sample.harness.shell"),
    "zephyr/subsys/console/echo": Build("subsys/console/echo", test="sample.console.echo"),
    "zephyr/subsys/console/getchar": Build("subsys/console/getchar", test="sample.console.getchar"),
    "zephyr/subsys/console/getline": Build("subsys/console/getline", test="sample.console.getline"),
    "zephyr/drivers/uart/echo_bot": Build("drivers/uart/echo_bot", test="sample.drivers.uart"),
    # Wave 1c: overlays of ours, no part.
    "zephyr/basic/threads": Build("basic/threads", overlays=("test/zephyr_samples/overlays/threads_led1.overlay",)),
    "zephyr/basic/custom_dts_binding": Build(
        "basic/custom_dts_binding", overlays=("test/zephyr_samples/overlays/load_switch_gp25.overlay",)),
    # Wave 1d: parts whose models a Zephyr driver already runs against (the
    # rig's device images), on the nodes those images use
    # (devices/<part>/test/dut/rp2.overlay), with the sample's alias.
    # samples/sensor/pressure_polling: build_only upstream (judged by our own
    # criterion).
    "zephyr/sensor/pressure_polling": Build(
        "sensor/pressure_polling", test="sample.sensor.pressure_sensor",
        overlays=("devices/bmp388/test/dut/rp2.overlay", "test/zephyr_samples/overlays/pressure_sensor_bmp388.overlay"),
        confs=("devices/bmp388/test/dut/rp2.conf",)),
    # samples/drivers/gnss: the rig's neo_m8n node on uart1 (DUT TX GP8, RX
    # GP9), which also sets the gnss alias.
    "zephyr/drivers/gnss": Build(
        "drivers/gnss", test="sample.drivers.gnss",
        overlays=("devices/neo_m8n/test/dut/uart1/rp2.overlay",),
        confs=("test/zephyr_samples/overlays/gnss_neo_m8n.conf",)),
    # samples/drivers/lora/{send,receive}: the rig's sx1262 node on SPI0 and
    # its LoRaMAC-node backend.
    **{f"zephyr/drivers/lora/{s}": Build(f"drivers/lora/{s}", test=f"sample.driver.lora.{s}",
                                         overlays=("devices/sx126x/test/dut/rp2.overlay",),
                                         confs=("devices/sx126x/test/dut/rp2.conf",))
       for s in ("send", "receive")},
    # samples/subsys/lorawan/class_a: its EU868 test, with the rig's sx1262.
    "zephyr/subsys/lorawan/class_a": Build("subsys/lorawan/class_a", test="sample.lorawan.class_a.eu868",
                                           overlays=("devices/sx126x/test/dut/rp2.overlay",),
                                           confs=("devices/sx126x/test/dut/rp2.conf",)),
    # samples/drivers/modem/at_client: console on uart0, the modem on uart1.
    "zephyr/drivers/modem/at_client": Build("drivers/modem/at_client",
                                            overlays=("test/zephyr_samples/overlays/at_client_uart1.overlay",)),
    # Wave 1e: parts so far driven only by pico-examples programs, on I2C0 at
    # GP4/GP5 (overlays/i2c0.overlay) with a node of ours each. mpu6050 and
    # ina219 are build_only upstream and lis2dh's harness is "sensor": our
    # own criteria judge them.
    **{f"zephyr/sensor/{sample}": Build(f"sensor/{sample}", test=test, overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", f"test/zephyr_samples/overlays/{node}.overlay"))
       for sample, test, node in (("mpu6050", "sample.sensor.mpu6050", "mpu6050_68"),
                                  ("lis2dh", "sample.sensor.lis2dh", "lis2dh_18"),
                                  ("ina219", "sample.drivers.ina219", "ina219_40"),
                                  ("thermometer", "sample.sensor.thermometer", "mcp9808_ambient"))},
    # samples/sensor/accel_polling: a LIS2DH-family part as accel0.
    "zephyr/sensor/accel_polling": Build("sensor/accel_polling", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/accel0_lis2dh.overlay")),
    # samples/sensor/bme280: on the Pico 1 its own boards/rpi_pico.overlay, a
    # BME280 on SPI0 (SCK GP18, MOSI GP19, MISO GP16) with CS on GP17.
    "zephyr/sensor/bme280": Build("sensor/bme280", test="sample.sensor.bme280", boards=("pico",),
                                  other_boards="the sample's rpi_pico2 build needs an SPI0 overlay of ours"),
    # samples/sensor/ds18b20 and drivers/w1/scanner: a DS18B20 on a GPIO
    # 1-Wire bus on GP15; upstream runs them on serial or I2C bus masters.
    # The scanner's max test is the one that adds no overlay of its own.
    "zephyr/sensor/ds18b20": Build("sensor/ds18b20", test="sample.sensor.ds18b20.w1_serial",
                                   overlays=("test/zephyr_samples/overlays/w1_gp15_ds18b20.overlay",)),
    "zephyr/drivers/w1/scanner": Build("drivers/w1/scanner", test="sample.drivers.w1.scanner.max",
                                       overlays=("test/zephyr_samples/overlays/w1_gp15_ds18b20.overlay",)),
    # samples/subsys/testsuite: ztest images that upstream builds only for
    # native_sim (integration, build_only) and qemu_x86 (benchmark).
    "zephyr/subsys/testsuite/integration": Build("subsys/testsuite/integration", test="sample.testing.ztest"),
    "zephyr/subsys/testsuite/benchmark": Build("subsys/testsuite/benchmark", test="sample.testing.ztest.benchmark"),
    # samples/net/sockets/socketpair: its first test, on a "net" harness with
    # no patterns (judged by our own criterion).
    "zephyr/net/sockets/socketpair": Build("net/sockets/socketpair", test="sample.net.sockets.socketpair"),
    # The Pico 1 builds take the samples' own boards/rpi_pico.overlay (PWM on
    # the LED); the Pico 2 builds need an overlay of ours.
    **{f"zephyr/{s}": Build(s, boards=("pico",), other_boards="the sample has no rpi_pico2 overlay yet")
       for s in ("basic/blinky_pwm", "basic/fade_led")},
    # samples/drivers/rtc: the RP2040's on-chip RTC, the rpi_pico board's rtc
    # alias (boards/raspberrypi/rpi_pico/rpi_pico-common.dtsi); upstream's
    # platform_allow runs the sample on RP2040 boards only with an external
    # RTC shield. The RP2350 has no RTC: its build needs a pcf8523 part.
    "zephyr/drivers/rtc": Build("drivers/rtc", test="sample.drivers.rtc", boards=("pico",),
                                other_boards="the RP2350 has no RTC; its build takes a pcf8523 part (wave 1e)"),
    # samples/drivers/adc/adc_dt: its rpi_pico overlay's ADC channels;
    # upstream's platform_allow leaves the RP2 boards out.
    "zephyr/drivers/adc/adc_dt": Build("drivers/adc/adc_dt", test="sample.drivers.adc.adc_dt"),
}
