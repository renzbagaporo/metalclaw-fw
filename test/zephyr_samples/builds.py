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

# The storage partition test/zephyr_samples/overlays/storage_<board>.overlay
# makes, as the flash address and size the image's erased block covers.
STORAGE = {
    "pico": (0x101F0000, 0x10000),
    "pico2": (0x103F0000, 0x10000),
}


@dataclass(frozen=True)
class Build:
    # The sample's directory under samples/.
    sample: str
    # The yaml test to build, or None for its first test that runs on a
    # console, ztest or shell harness and is not build_only; "" for a
    # sample with no yaml, built as it is.
    test: str | None = None
    # Extra CMake cache entries ("NAME=value"), after the test's own.
    # CMake cache entries; a "platform:<board>:" one only on that board, as
    # twister's extra_args.
    args: tuple[str, ...] = ()
    # Devicetree overlays and Kconfig fragments of ours, repository-relative,
    # added after the test's own (EXTRA_DTC_OVERLAY_FILE, EXTRA_CONF_FILE):
    # the parts' nodes and the aliases the sample looks them up by.
    overlays: tuple[str, ...] = ()
    confs: tuple[str, ...] = ()
    # A storage partition (overlays/storage_<board>.overlay), erased by
    # each flash of the image.
    storage: bool = False
    # A second test of the same sample, as its own target:
    # zephyr/<sample>@<variant>.
    variant: str = ""
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
    # Wave 1a samples that need modules of the samples workspace
    # (test/zephyr_samples/west.yml): mipi-sys-t, mbedtls and
    # tf-psa-crypto, cmsis-dsp, nanopb, and the optional group's
    # tflite-micro and chre.
    **_console("subsys/logging/syst", "modules/cmsis_dsp/moving_average", "modules/nanopb",
               "modules/tflite-micro/hello_world", "modules/chre"),
    # subsys/uuid runs upstream only on native_sim and qemu_x86. On the
    # Picos its PSA SHA-1 and base64 overrun the boards' 1 KiB main stack
    # (CONFIG_STACK_SENTINEL reports it), and the thread switched to next
    # faults at PC 0: built with 4 KiB.
    "zephyr/subsys/uuid": Build("subsys/uuid", args=("CONFIG_MAIN_STACK_SIZE=4096",)),
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
    **_console("subsys/shell/shell_module", "drivers/flash_shell", "arch/mpu/mpu_test"),
    # shell_module's minimal, getopt, robot and login tests (judged by
    # commands of ours: their yamls' harnesses are keyboard and robot).
    **{f"zephyr/subsys/shell/shell_module@{v}": Build("subsys/shell/shell_module",
                                                      test=f"sample.shell.shell_module.{v}", variant=v)
       for v in ("minimal", "getopt", "robot", "login")},
    "zephyr/subsys/testsuite/pytest/shell": Build("subsys/testsuite/pytest/shell", test="sample.harness.shell"),
    "zephyr/subsys/console/echo": Build("subsys/console/echo", test="sample.console.echo"),
    "zephyr/subsys/console/getchar": Build("subsys/console/getchar", test="sample.console.getchar"),
    "zephyr/subsys/console/getline": Build("subsys/console/getline", test="sample.console.getline"),
    "zephyr/drivers/uart/echo_bot": Build("drivers/uart/echo_bot", test="sample.drivers.uart"),
    "zephyr/subsys/shell/devmem_load": Build("subsys/shell/devmem_load", test="sample.devmem_load.uart.interrupt"),
    # Wave 1c: overlays of ours, no part.
    "zephyr/basic/threads": Build("basic/threads", overlays=("test/zephyr_samples/overlays/threads_led1.overlay",)),
    "zephyr/basic/custom_dts_binding": Build(
        "basic/custom_dts_binding", overlays=("test/zephyr_samples/overlays/load_switch_gp25.overlay",)),
    # basic/rgb_led (harness led) and servo_motor (harness motor): PWM
    # outputs of ours on GP13-15; nothing on the bench reads PWM yet, so the
    # console judges them.
    "zephyr/basic/rgb_led": Build("basic/rgb_led", test="sample.basic.rgb_led",
                                  overlays=("test/zephyr_samples/overlays/rgb_pwm_gp13_15.overlay",)),
    "zephyr/basic/servo_motor": Build("basic/servo_motor", test="sample.basic.servo_motor",
                                      overlays=("test/zephyr_samples/overlays/servo_pwm_gp15.overlay",)),
    # Wave 1d: parts whose models a Zephyr driver already runs against (the
    # rig's device images), on the nodes those images use
    # (devices/<part>/test/dut/rp2.overlay), with the sample's alias.
    # samples/sensor/pressure_polling: build_only upstream (judged by our own
    # criterion).
    "zephyr/sensor/pressure_polling": Build(
        "sensor/pressure_polling", test="sample.sensor.pressure_sensor",
        overlays=("devices/bmp388/test/dut/rp2.overlay", "test/zephyr_samples/overlays/pressure_sensor_bmp388.overlay"),
        confs=("devices/bmp388/test/dut/rp2.conf",)),
    # samples/sensor/pressure_interrupt: build_only upstream (judged by our
    # own criterion); the same BMP388 with its INT pad on GP15, its trigger
    # on (the rig's DUT config turns it off), and only the data-ready trigger
    # the BMP388 driver offers.
    "zephyr/sensor/pressure_interrupt": Build(
        "sensor/pressure_interrupt", test="sample.sensor.pressure_interrupt",
        overlays=("devices/bmp388/test/dut/rp2.overlay", "test/zephyr_samples/overlays/pressure_sensor_bmp388.overlay",
                  "test/zephyr_samples/overlays/bmp388_int_gp15.overlay"),
        confs=("devices/bmp388/test/dut/rp2.conf",),
        args=("CONFIG_BMP388_TRIGGER_GLOBAL_THREAD=y", "CONFIG_PRESSURE_THRESHOLD=n", "CONFIG_PRESSURE_DELTA=n")),
    # samples/sensor/6dof_motion_drdy: build_only upstream, for an
    # ICM-42670-P; the ICM-42688 here, INT1 on GP15, its trigger on (the
    # part's DUT config turns it off). Its CONFIG_LOG would defer printk,
    # whose boot banner the trigger's printf lines then cut into or, on the
    # Pico 2, the log drops: printk prints at once instead.
    "zephyr/sensor/6dof_motion_drdy": Build(
        "sensor/6dof_motion_drdy", test="sample.sensor.6dof_motion_drdy",
        overlays=("devices/icm4268x/test/dut/rp2.overlay", "test/zephyr_samples/overlays/icm42688_int1_gp15.overlay"),
        confs=("devices/icm4268x/test/dut/rp2.conf",),
        args=("CONFIG_ICM4268X_TRIGGER_GLOBAL_THREAD=y", "CONFIG_LOG_PRINTK=n")),
    # samples/net/cellular_modem: an A76XX on uart1. Its endpoint is the
    # sample's Kconfig option: the model's DNS answers a dotted IPv4 name
    # with that address, and its network answers on every address.
    "zephyr/net/cellular_modem": Build(
        "net/cellular_modem", test="sample.net.cellular_modem",
        overlays=("test/zephyr_samples/overlays/uart1_gp8_gp9.overlay",
                  "test/zephyr_samples/overlays/cellular_modem_a76xx_uart1.overlay"),
        # The extra CMUX work buffer turns on MSC flow control a frame
        # before a DLCI's receive buffer fills (MODEM_CMUX_MSC_FC_THRESHOLD),
        # as the rig's own cellular DUT has it: without it the 1024-byte
        # echoes overran it, the sample's debug logging slowing the reader.
        args=('CONFIG_SAMPLE_CELLULAR_MODEM_ENDPOINT_HOSTNAME="203.0.113.7"',
              "CONFIG_MODEM_CMUX_WORK_BUFFER_SIZE_EXTRA=512",
              # The RP2040 has no entropy source Zephyr drives (as
              # test/rig/dut/boards/rpi_pico.conf); the Pico 2's TRNG is one.
              "platform:rpi_pico:CONFIG_TEST_RANDOM_GENERATOR=y")),
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
                                  ("thermometer", "sample.sensor.thermometer", "mcp9808_ambient"),
                                  # As upstream's RP2040 run (the adafruit_mcp9808
                                  # shield): no alert pin, so its trigger
                                  # options fall away and it polls.
                                  ("jc42", "sample.sensor.jc42", "mcp9808_ambient"))},
    # samples/sensor/accel_trig: an MPU-6050 as accel0, its INT pin on GP15,
    # for the data-ready trigger the sample sets by default.
    "zephyr/sensor/accel_trig": Build("sensor/accel_trig", test="sample.sensor.accel_trig", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/accel0_mpu6050_int_gp15.overlay")),
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
    # samples/basic/button (build_only upstream) and gpio/button_interrupt
    # (harness "button"): a gpio-keys sw0 on GP15 that the harness presses.
    "zephyr/basic/button": Build("basic/button", test="sample.basic.button",
                                 overlays=("test/zephyr_samples/overlays/sw0_gp15.overlay",)),
    "zephyr/drivers/gpio/button_interrupt": Build("drivers/gpio/button_interrupt",
                                                  test="sample.gpio.button_interrupt",
                                                  overlays=("test/zephyr_samples/overlays/sw0_gp15.overlay",)),
    # samples/drivers/ht16k33: its yaml's harness is "TBD".
    "zephyr/drivers/ht16k33": Build("drivers/ht16k33", test="sample.drivers.ht16k33", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ht16k33_70.overlay")),
    # samples/drivers/display: its builtin-display test, with an SSD1306 of
    # ours as zephyr,display.
    "zephyr/drivers/display": Build("drivers/display", test="sample.display.builtin", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ssd1306_3c.overlay")),
    # samples/drivers/misc/grove_display: a Grove LCD RGB Backlight on I2C0.
    "zephyr/drivers/misc/grove_display": Build("drivers/misc/grove_display", test="sample.drivers.misc.grove_display",
                                               overlays=("test/zephyr_samples/overlays/i2c0.overlay",
                                                         "test/zephyr_samples/overlays/grove_lcd_rgb_3e.overlay")),
    # samples/drivers/led/pca9633: its app.overlay puts the part on
    # arduino_i2c, which the rpi_pico_uno_flexypin shield maps to I2C0
    # (GP4/GP5).
    "zephyr/drivers/led/pca9633": Build("drivers/led/pca9633", test="sample.drivers.led.pca9633",
                                        args=("SHIELD=rpi_pico_uno_flexypin",)),
    # samples/sensor/magn_polling and magn_trig: an MPU-9250's AK8963 as
    # magn0, its data-ready on GP15.
    **{f"zephyr/sensor/{sample}": Build(f"sensor/{sample}", test=test, overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/magn0_mpu9250_68.overlay"))
       for sample, test in (("magn_polling", "sample.sensor.magn_polling"),
                            ("magn_trig", "sample.sensor.magn_trigger"))},
    # magn_polling again with an LSM303DLHC's magnetometer at 0x1E as magn0.
    "zephyr/sensor/magn_polling@lsm303dlhc": Build(
        "sensor/magn_polling", test="sample.sensor.magn_polling", variant="lsm303dlhc", overlays=(
            "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/magn0_lsm303dlhc_1e.overlay")),
    # samples/drivers/counter/maxim_ds3231: build_only upstream (judged by
    # our own criterion); a DS3231 at 0x68 on I2C0, INT/SQW on GP15.
    "zephyr/drivers/counter/maxim_ds3231": Build(
        "drivers/counter/maxim_ds3231", test="sample.basic.maxim_ds3231", overlays=(
            "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ds3231_68_isw_gp15.overlay")),
    # samples/drivers/biometrics/fingerprint: a ZFM-20 on uart1 (GP8 TX,
    # GP9 RX), where its app.overlay puts the zfm-x0; its yaml allows only
    # native_sim.
    "zephyr/drivers/biometrics/fingerprint": Build(
        "drivers/biometrics/fingerprint", test="sample.drivers.biometrics.fingerprint", overlays=(
            "test/zephyr_samples/overlays/uart1_gp8_gp9.overlay",
            "test/zephyr_samples/overlays/fingerprint_zfm20_uart1.overlay")),
    # samples/drivers/dac: the adafruit_mcp4728 shield's node, an MCP4728
    # at 0x60 on I2C0, as zephyr,user's dac (the shield itself targets the
    # Feather's zephyr_i2c).
    "zephyr/drivers/dac@mcp4728": Build("drivers/dac", test="sample.drivers.dac", variant="mcp4728", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/dac_mcp4728_60.overlay")),
    # The same with the adafruit_ad5693r shield's node, an AD5693R at 0x4C.
    "zephyr/drivers/dac@ad5693r": Build("drivers/dac", test="sample.drivers.dac", variant="ad5693r", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/dac_ad5693r_4c.overlay")),
    # samples/sensor/hts221: ST's HTS221 at 0x5F on I2C0, polled.
    "zephyr/sensor/hts221": Build("sensor/hts221", test="sample.sensor.hts221", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/hts221_5f.overlay")),
    # samples/drivers/haptics/drv2605: the adafruit_drv2605l shield's node,
    # a DRV2605L at 0x5A on I2C0; its yaml is build_only.
    "zephyr/drivers/haptics/drv2605": Build("drivers/haptics/drv2605", test="sample.haptics.drv2605", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/haptic_drv2605_5a.overlay")),
    # samples/drivers/led/lp50xx: a TI LP5012 at 0x14 on I2C0, four RGB
    # LEDs. Its printk lines went through the deferred log, which dropped
    # the banner and the controller line: printk prints at once instead.
    "zephyr/drivers/led/lp50xx": Build("drivers/led/lp50xx", test="sample.drivers.led.lp50xx", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/lp5012_14.overlay"),
        args=("CONFIG_LOG_PRINTK=n",)),
    # samples/drivers/led/is31fl3216a: an IS31FL3216A at 0x74 on I2C0.
    "zephyr/drivers/led/is31fl3216a": Build("drivers/led/is31fl3216a", test="sample.drivers.led.is31fl3216a",
                                            overlays=("test/zephyr_samples/overlays/i2c0.overlay",
                                                      "test/zephyr_samples/overlays/is31fl3216a_74.overlay")),
    # samples/drivers/led/sx1509b_intensity: an SX1509B at 0x3E on I2C0
    # with the thingy52's RGB LEDs on its I/O 5-7.
    "zephyr/drivers/led/sx1509b_intensity": Build(
        "drivers/led/sx1509b_intensity", test="sample.drivers.led.sx1509b_intensity", overlays=(
            "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/sx1509b_3e_leds.overlay")),
    # samples/sensor/vl53l0x: a VL53L0X at 0x29 on I2C0.
    "zephyr/sensor/vl53l0x": Build("sensor/vl53l0x", test="sample.sensor.vl53l0x", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/vl53l0x_29.overlay")),
    # samples/drivers/led/sct2024: an SCT2024 on SPI0, LA/ GP20, OE/ GP21.
    "zephyr/drivers/led/sct2024": Build("drivers/led/sct2024", test="sample.drivers.led.sct2024",
                                        overlays=("test/zephyr_samples/overlays/sct2024_spi0.overlay",)),
    # samples/drivers/led/is31fl319x: an IS31FL3194 at 0x53 on I2C0.
    "zephyr/drivers/led/is31fl319x": Build("drivers/led/is31fl319x", test="sample.drivers.led.is31fl319x", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/is31fl3194_53.overlay")),
    # samples/drivers/led/is31fl3733: an IS31FL3733 at 0x50 on I2C0.
    "zephyr/drivers/led/is31fl3733": Build("drivers/led/is31fl3733", test="sample.drivers.led.is31fl3733", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/is31fl3733_50.overlay")),
    # samples/drivers/led/lp3943: a TI LP3943 at 0x60 on I2C0.
    "zephyr/drivers/led/lp3943": Build("drivers/led/lp3943", test="sample.drivers.led.lp3943", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/lp3943_60.overlay")),
    # samples/drivers/led/lp5562: a TI LP5562 at 0x30 on I2C0.
    "zephyr/drivers/led/lp5562": Build("drivers/led/lp5562", test="sample.drivers.led.lp5562", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/lp5562_30.overlay")),
    # samples/drivers/led/lp5569: a TI LP5569 at 0x32 on I2C0.
    "zephyr/drivers/led/lp5569": Build("drivers/led/lp5569", test="sample.drivers.led.lp5569", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/lp5569_32.overlay")),
    # samples/drivers/eeprom: an AT24C02C at 0x50 on I2C0 as eeprom-0.
    "zephyr/drivers/eeprom": Build("drivers/eeprom", test="sample.drivers.eeprom", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/eeprom0_at24c02_50.overlay")),
    # samples/sensor/max6675: a MAX6675 on SPI0.
    "zephyr/sensor/max6675": Build("sensor/max6675", test="sample.sensor.max6675",
                                   overlays=("test/zephyr_samples/overlays/max6675_spi0.overlay",)),
    # samples/sensor/lsm6dsl: ST's LSM6DSL at 0x6A on I2C0, INT1 on GP15
    # for the data-ready trigger its prj.conf turns on.
    "zephyr/sensor/lsm6dsl": Build("sensor/lsm6dsl", test="sample.sensor.lsm6dsl", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/lsm6dsl_6a.overlay")),
    # samples/sensor/lsm6dso: ST's LSM6DSO at 0x6A on I2C0, INT1 on GP15
    # for the data-ready trigger its prj.conf turns on.
    "zephyr/sensor/lsm6dso": Build("sensor/lsm6dso", test="sample.sensor.lsm6dso", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/lsm6dso_6a.overlay")),
    # samples/sensor/lps22hb: ST's LPS22HB at 0x5C on I2C0.
    "zephyr/sensor/lps22hb": Build("sensor/lps22hb", test="sample.sensor.lps22hb", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/lps22hb_5c.overlay")),
    # Vishay's VEML6031X00 and VEML6046X00 at 0x29 on I2C0.
    **{f"zephyr/sensor/{sample}": Build(f"sensor/{sample}", test=f"sample.sensor.{sample}", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", f"test/zephyr_samples/overlays/{sample}_29.overlay"))
       for sample in ("veml6031", "veml6046")},
    # Sensirion's SHT3x-DIS and SHT4x at 0x44 on I2C0.
    **{f"zephyr/sensor/{sample}": Build(f"sensor/{sample}", test=test, overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", f"test/zephyr_samples/overlays/{sample}_44.overlay"))
       for sample, test in (("sht3xd", "sample.sensor.sht3xd"), ("sht4x", "sample.sensor.sht4x"))},
    # TI's TMP108, TMP112 and TMP116 at 0x48 on I2C0.
    **{f"zephyr/sensor/{sample}": Build(f"sensor/{sample}", test=test, overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", f"test/zephyr_samples/overlays/{node}.overlay"))
       for sample, test, node in (("tmp108", "sample.sensor.tmp108", "tmp108_48"),
                                  ("tmp112", "sample.sensor.tmp112", "tmp112_48"),
                                  ("tmp11x", "sample.sensor.tmp11x", "tmp116_48"))},
    # The CAN samples: an MCP2515 on SPI0 as zephyr,canbus, in its own
    # loopback mode (the bench has no CAN bus).
    **{f"zephyr/{sample}": Build(sample, test=test, overlays=("test/zephyr_samples/overlays/mcp2515_spi0.overlay",))
       for sample, test in (("drivers/can/counter", "sample.drivers.can.counter"),
                            ("drivers/can/babbling", "sample.drivers.can.babbling"),
                            ("subsys/canbus/isotp", "sample.subsys.canbus.isotp"),
                            ("net/sockets/can", None))},
    # samples/drivers/spi_flash: a W25Q16JV on SPI0, its sector at 0xff000.
    "zephyr/drivers/spi_flash": Build("drivers/spi_flash", test="sample.drivers.spi.flash",
                                      overlays=("test/zephyr_samples/overlays/w25q16jv_spi0.overlay",)),
    # samples/drivers/spi_flash_at45: an AT45DB041E on SPI0, the test region
    # at 0xfe00; the test's own page-layout and PM conf files.
    "zephyr/drivers/spi_flash_at45": Build("drivers/spi_flash_at45", test="sample.drivers.spi.flash.at45",
                                           overlays=("test/zephyr_samples/overlays/at45db041e_spi0.overlay",)),
    # samples/drivers/spi_bitbang: spibb0 on GP12-15, MOSI looped to MISO.
    "zephyr/drivers/spi_bitbang": Build("drivers/spi_bitbang",
                                        overlays=("test/zephyr_samples/overlays/spibb0_gp12_15.overlay",)),
    # samples/drivers/uart/passthrough (harness keyboard): uart1 looped back.
    "zephyr/drivers/uart/passthrough": Build("drivers/uart/passthrough", test="sample.drivers.uart.passthrough",
                                             overlays=("test/zephyr_samples/overlays/passthrough_uart1.overlay",)),
    # samples/drivers/led/led_strip: its yaml names a fixture and no patterns.
    "zephyr/drivers/led/led_strip": Build("drivers/led/led_strip", test="sample.drivers.led_strip",
                                          boards=("pico",),
                                          other_boards="the Pico 2 build must also free GP6 from I2C1",
                                          overlays=("test/zephyr_samples/overlays/ws2812_pio_gp6.overlay",)),
    # samples/drivers/i2c/rtio_loopback: I2C1 controller, I2C0 target.
    "zephyr/drivers/i2c/rtio_loopback": Build("drivers/i2c/rtio_loopback", test="sample.drivers.i2c.rtio_loopback",
                                              overlays=("test/zephyr_samples/overlays/i2c0.overlay",
                                                        "test/zephyr_samples/overlays/i2c_loopback.overlay")),
    # Storage samples: a storage partition of ours, empty at each flash.
    # kvss/nvs's reboot counter and littlefs's boot count then start from 0
    # in every session, not from wherever the last one left them.
    "zephyr/subsys/kvss/nvs": Build("subsys/kvss/nvs", test="sample.nvs.basic", storage=True),
    # littlefs's prj.conf turns on CONFIG_DEBUG (-Og), and so built the
    # RP2040 hangs in littlefs's first format of the partition, where the
    # same build without it formats, mounts and counts (and fs/format,
    # built without it, formats the same partition): built without it.
    "zephyr/subsys/fs/littlefs": Build("subsys/fs/littlefs", test="sample.filesystem.littlefs", storage=True,
                                       args=("CONFIG_DEBUG=n",)),
    "zephyr/subsys/settings": Build("subsys/settings", test="sample.subsys.settings.nvs", storage=True),
    "zephyr/subsys/kvss/zms": Build("subsys/kvss/zms", test="sample.zms.basic", storage=True),
    # samples/subsys/fs/format: both its tests, build-only upstream (judged
    # by our own criterion): littlefs on the storage partition, and FAT on a
    # RAM disk (prj_ram.conf, ramdisk.overlay).
    "zephyr/subsys/fs/format": Build("subsys/fs/format", test="sample.filesystem.format.littlefs", storage=True),
    "zephyr/subsys/fs/format@fat_fs": Build("subsys/fs/format", test="sample.filesystem.format.fat_fs",
                                            variant="fat_fs"),
    # samples/subsys/shell/fs: the file-system shell over littlefs on the
    # storage partition (judged by commands of ours).
    "zephyr/subsys/shell/fs": Build("subsys/shell/fs", test="sample.filesystem.shell", storage=True),
    # samples/drivers/auxdisplay_digits: build-only upstream (native_sim);
    # one digit of ours, judged by its pins.
    "zephyr/drivers/auxdisplay_digits": Build("drivers/auxdisplay_digits", test="sample.drivers.auxdisplay_digits",
                                              overlays=("test/zephyr_samples/overlays/"
                                                        "auxdisplay_digits_gp2_10.overlay",)),
    # Userspace: the three userspace samples and the logger's user-mode
    # test, with the MPU on (the Picos' board configs leave it off).
    # gperf on the build host generates their kernel-object hash table.
    **{f"zephyr/userspace/{s}": Build(f"userspace/{s}", args=("CONFIG_ARM_MPU=y",))
       for s in ("hello_world_user", "prod_consumer", "shared_mem")},
    # Not built: the logger's user-mode test (sample.logger.usermode). The
    # Pico boards leave the MPU off; with CONFIG_ARM_MPU=y forced, it runs
    # its kernel-thread demo and then HardFaults in its user thread, which
    # touches kernel data (__aeabi_read_tp's load of z_arm_tls_ptr, then
    # log_set_tag's copy into the logger's kernel buffer), on silicon as in
    # the emulator. Upstream never runs it (harness keyboard).
    # samples/basic/minimal: its runtime test, which upstream runs on ARC
    # (common-runtime.conf, no-timers.conf): no kernel timers, one printk.
    "zephyr/basic/minimal": Build("basic/minimal", test="sample.minimal.arc.runtime"),
    # samples/psa/{its,persistent_key}: PSA secure storage on the storage
    # partition. Zephyr has no entropy driver for the RP2 chips, so their
    # entropy_not_secure tests.
    **{f"zephyr/psa/{s}": Build(f"psa/{s}", test=f"sample.psa.{s}.secure_storage.entropy_not_secure", storage=True)
       for s in ("its", "persistent_key")},
    # samples/subsys/display/cfb and cfb_custom_font: the character
    # framebuffer on the SSD1306 of drivers/display; their tests (harness
    # display) name other boards' panels, so the ssd16xx test, which adds
    # nothing of its own.
    "zephyr/subsys/display/cfb": Build("subsys/display/cfb", test="sample.display.cfb.ssd16xx", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ssd1306_3c.overlay")),
    "zephyr/subsys/display/cfb_shell": Build("subsys/display/cfb_shell", test="sample.display.cfb_shell.ssd16xx", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ssd1306_3c.overlay")),
    "zephyr/subsys/display/cfb_custom_font": Build(
        "subsys/display/cfb_custom_font", test="sample.display.cfb_custom_font.ssd16xx", overlays=(
            "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ssd1306_3c.overlay")),
    # samples/drivers/adc/adc_sequence: ADC inputs 0 and 1 (GP26, GP27);
    # upstream lists other boards.
    "zephyr/drivers/adc/adc_sequence": Build("drivers/adc/adc_sequence", test="sample.drivers.adc.adc_sequence",
                                             overlays=("test/zephyr_samples/overlays/adc0_ch0_ch1.overlay",)),
    # samples/sensor/sm351lt (harness sensor): a magnetic switch on GP15
    # that the harness opens and closes.
    "zephyr/sensor/sm351lt": Build("sensor/sm351lt", test="sample.sensor.sm351lt",
                                   overlays=("test/zephyr_samples/overlays/sm351lt_gp15.overlay",)),
    # samples/sensor/soc_voltage: the Picos' own VSYS divider on ADC input 3.
    "zephyr/sensor/soc_voltage": Build("sensor/soc_voltage",
                                       overlays=("test/zephyr_samples/overlays/vsys_divider.overlay",)),
    # samples/sensor/dht_polling: a BME280 on I2C0 as dht0.
    "zephyr/sensor/dht_polling": Build("sensor/dht_polling", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/dht0_bme280_76.overlay")),
    # samples/subsys/input/input_dump (build_only upstream): logs every
    # input event, from sw0 on GP15 and a gpio-qdec wheel on GP10/GP11.
    "zephyr/subsys/input/input_dump": Build("subsys/input/input_dump", test="sample.input.input_dump", overlays=(
        "test/zephyr_samples/overlays/sw0_gp15.overlay", "test/zephyr_samples/overlays/qdec_gp10_11.overlay")),
    # input_dump again with an FT5336 touch controller at 0x38 on I2C0.
    "zephyr/subsys/input/input_dump@ft5336": Build(
        "subsys/input/input_dump", test="sample.input.input_dump", variant="ft5336", overlays=(
            "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/touch_ft5336_38.overlay")),
    # samples/sensor/sensor_shell: the keyboard test, with an INA237 of ours.
    "zephyr/sensor/sensor_shell": Build("sensor/sensor_shell", test="sample.sensor.shell", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ina237_40.overlay")),
    # samples/drivers/stepper/generic (harness stepper): a TMC2209 on GPIOs
    # of ours, its mode button sw0 on GP15.
    "zephyr/drivers/stepper/generic": Build("drivers/stepper/generic", test="sample.drivers.stepper.generic",
                                            overlays=("test/zephyr_samples/overlays/sw0_gp15.overlay",
                                                      "test/zephyr_samples/overlays/stepper_gp10_14.overlay")),
    # samples/sensor/grove_temperature and light_polling's grove test
    # (harness grove): the Grove sensors on ADC input 0, GP26.
    "zephyr/sensor/grove_temperature": Build(
        "sensor/grove_temperature", test="sample.sensor.grove_temperature",
        overlays=("test/zephyr_samples/overlays/grove_temperature_adc0.overlay",)),
    "zephyr/sensor/light_polling": Build(
        "sensor/light_polling", test="sample.sensor.light_polling.grove",
        overlays=("test/zephyr_samples/overlays/grove_light_adc0.overlay",)),
    # samples/subsys/zbus/uart_bridge: zbus messages bridged out of uart1
    # (GP8/GP9), as its nrf52840dk overlay enables that board's uart1.
    "zephyr/subsys/zbus/uart_bridge": Build("subsys/zbus/uart_bridge",
                                            overlays=("test/zephyr_samples/overlays/uart1_gp8_gp9.overlay",)),
    # samples/drivers/fuel_gauge: a composite gauge over an INA219.
    "zephyr/drivers/fuel_gauge": Build("drivers/fuel_gauge", test="sample.sensor.fuel_gauge", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ina219_40.overlay",
        "test/zephyr_samples/overlays/fuel_gauge_ina219.overlay"),
        confs=("test/zephyr_samples/overlays/sensor_i2c.conf",)),
    # samples/drivers/auxdisplay: an HD44780 behind a PCF8574 backpack.
    "zephyr/drivers/auxdisplay": Build("drivers/auxdisplay", test="sample.drivers.auxdisplay", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/auxdisplay_pcf8574_27.overlay"),
        confs=("test/zephyr_samples/overlays/auxdisplay_pcf8574.conf",)),
    # samples/subsys/sensing/simple: its sensing node on an MPU-6050 (I2C)
    # and an ICM-42688 (SPI); upstream builds it for native_sim's BMI160s.
    "zephyr/subsys/sensing/simple": Build("subsys/sensing/simple", test="sample.sensing.simple", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "devices/icm4268x/test/dut/rp2.overlay",
        "test/zephyr_samples/overlays/sensing_mpu6050_icm42688.overlay"),
        confs=("devices/icm4268x/test/dut/rp2.conf", "test/zephyr_samples/overlays/sensing_simple.conf")),
    # samples/boards/raspberrypi/rpi_pico/uart_pio: no sample.yaml; its own
    # rpi_pico overlay's PIO UARTs (judged by our own criterion).
    "zephyr/boards/raspberrypi/rpi_pico/uart_pio": Build("boards/raspberrypi/rpi_pico/uart_pio", test="",
                                                         boards=("pico",),
                                                         other_boards="its Pico 2 overlay's pins need checking"),
    # samples/subsys/display/lvgl: its gui test (harness none) on the SSD1306
    # of drivers/display, monochrome.
    "zephyr/subsys/display/lvgl": Build("subsys/display/lvgl", test="sample.display.lvgl.gui", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ssd1306_3c.overlay"),
                                        confs=("test/zephyr_samples/overlays/lvgl_ssd1306.conf",)),
    # samples/modules/lvgl/accelerometer_chart: the same panel, and a
    # LIS2DH as accel0 on I2C1.
    "zephyr/modules/lvgl/accelerometer_chart": Build("modules/lvgl/accelerometer_chart",
                                                     test="sample.modules.lvgl.accelerometer_chart", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ssd1306_3c.overlay",
        "test/zephyr_samples/overlays/accel0_lis2dh_i2c1.overlay"),
        confs=("test/zephyr_samples/overlays/lvgl_ssd1306.conf",)),
    # samples/modules/lvgl/screen_transparency needs a display that takes
    # ARGB8888 (its README): an LED-strip matrix over the WS2812 chain.
    "zephyr/modules/lvgl/screen_transparency": Build(
        "modules/lvgl/screen_transparency", test="sample.modules.lvgl.screen_transparency", boards=("pico",),
        other_boards="the Pico 2 build must also free GP6 from I2C1",
        overlays=("test/zephyr_samples/overlays/ws2812_pio_gp6.overlay",
                  "test/zephyr_samples/overlays/led_matrix_32x40.overlay")),
    # samples/modules/lvgl/multi_display: the panel on I2C0 and a second
    # on I2C1, as the zephyr,displays list. With the sample's default demos
    # (music on the first, widgets on the others) the I2C0 panel is turned
    # on but never written (its data_bytes still 0 after 30 s) and the I2C1
    # panel takes three whole frames (3072 bytes) and no more, with nothing
    # on the console. Over SWD the core then sits in arch_system_halt after
    # a HardFault whose thread frame returns into RAM (PC 0x20012a4c, Thumb
    # bit clear): memory overwritten, the console's too, as no fault report
    # comes out even with immediate logging. The widgets or stress demo on
    # the first panel instead HardFaults soon after the banner. Upstream
    # runs the sample only on native_sim's colour displays. The demos run
    # in main, whose 4 KB stack (its prj.conf) they overflow: with 16 KB
    # both panels draw; with a stack sentinel at 4 KB the run fails as
    # before, the overflow wiping memory before the sentinel is checked.
    "zephyr/modules/lvgl/multi_display": Build("modules/lvgl/multi_display",
                                               test="sample.modules.lvgl.multi_display", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ssd1306_3c.overlay",
        "test/zephyr_samples/overlays/ssd1306_second_i2c1.overlay"),
        confs=("test/zephyr_samples/overlays/lvgl_ssd1306.conf",),
        args=("CONFIG_MAIN_STACK_SIZE=16384",)),
    # samples/modules/lvgl/demos: the widgets demo on the same panel.
    "zephyr/modules/lvgl/demos": Build("modules/lvgl/demos", test="sample.modules.lvgl.demo_widgets", overlays=(
        "test/zephyr_samples/overlays/i2c0.overlay", "test/zephyr_samples/overlays/ssd1306_3c.overlay"),
                                       confs=("test/zephyr_samples/overlays/lvgl_ssd1306.conf",)),
    # samples/subsys/testsuite: ztest images that upstream builds only for
    # native_sim (integration, build_only) and qemu_x86 (benchmark).
    "zephyr/subsys/testsuite/integration": Build("subsys/testsuite/integration", test="sample.testing.ztest"),
    "zephyr/subsys/testsuite/pytest/basic": Build("subsys/testsuite/pytest/basic", test="sample.twister.pytest"),
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
    # On the Pico 1 its yaml's second line (04:19:01, a second after it sets
    # 04:19:00) does not come: read over SWD without halting, the on-chip
    # RTC reaches :01 within 68 ms of boot, after a RUN reset too, and then
    # ticks every second, so the read a second later finds :02.
    "zephyr/drivers/rtc": Build("drivers/rtc", test="sample.drivers.rtc", boards=("pico",),
                                other_boards="the RP2350 has no RTC; its build takes a pcf8523 part (wave 1e)"),
    # samples/drivers/adc/adc_dt: its rpi_pico overlay's ADC channels;
    # upstream's platform_allow leaves the RP2 boards out.
    # The sample reads into an uninitialised uint32_t (src/main.c:37) that
    # the RP2 driver fills 16 bits of (adc_rpi_pico.c:231), so its upper half
    # is whatever the stack held (0x1000 on the Pico 2): locals start at zero.
    "zephyr/drivers/adc/adc_dt": Build("drivers/adc/adc_dt", test="sample.drivers.adc.adc_dt",
                                       args=("EXTRA_CFLAGS=-ftrivial-auto-var-init=zero",)),
}
