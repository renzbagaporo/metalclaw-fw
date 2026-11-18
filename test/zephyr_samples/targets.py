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
from test.zephyr_samples.parts import lora_frame

# The boot banner every sample prints first (kernel/banner.c, with
# CONFIG_BOOT_BANNER on and BOOT_BANNER_STRING "Booting Zephyr OS build" by
# default, kernel/Kconfig). It ends a console line, but need not start one:
# what the sample printed before the reset (a shell prompt, an unfinished
# line) stays on the line it starts in.
BANNER = r"\*\*\* Booting Zephyr OS build .* \*\*\*$"
# Its start, from which a run's forbidden output is judged: what the program
# from before the restart printed, the console read catches too.
BANNER_START = r"\*\*\* Booting Zephyr OS build "

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

# The lora receive sample's frames, one per lora_recv call.
RX_FRAMES = tuple(f"metalclaw {i}".encode() for i in range(4))

# By target: Spec fields that replace the ones the yaml gives.
OVERRIDES: dict[str, dict] = {
    # samples/drivers/led/pwm/src/main.c:80-140: on, off, a fade up and down
    # (CONFIG_FADE_DELAY steps), then blinking, with one-second pauses: on
    # the carrier the blinking starts 7 s in, so the loop ends after 10 s.
    "zephyr/drivers/led/pwm": {"seconds": 30.0},
    # samples/modules/lvgl/demos: its yaml asks for the heap summary in
    # brackets, but Zephyr 4.4.0 prints it without them
    # (lib/heap/heap_info.c:67, from lvgl_print_heap_info at
    # src/main.c:71-72); the line as printed.
    "zephyr/modules/lvgl/demos": {
        "console": (BANNER, r"^\d+ free bytes, \d+ allocated bytes, overhead = \d+ bytes"),
        "forbidden": (*FORBIDDEN, r"<err>", r"\[Error\]")},
    # samples/drivers/watchdog: feeds the watchdog five times, then lets it
    # reset the chip; its yaml expects the second boot.
    "zephyr/drivers/watchdog": {"forbidden": FAULTS, "seconds": 20.0},
    # samples/sensor/accel_polling: its yaml's line, which a reading in any
    # range matches, as the part's 252, -500, 1020 mg in m/s^2 (%12.6f),
    # through the LIS2DH driver's 1/1024 g digit (see lis2dh above).
    "zephyr/sensor/accel_polling": {
        "console": (BANNER, r"^\s*lis2dh@18 \[m/s\^2\]:    \(\s*2\.41\d*,\s*-4\.78\d*,\s*9\.76\d*\)$"),
    },
    # samples/sensor/bme280: its yaml's line, which any reading matches, as
    # the part's 21.37 °C, 99.887 kPa and 45.67 %RH (src/main.c:95-98),
    # within the decoder's fixed-point steps (it prints 99.882812 kPa).
    "zephyr/sensor/bme280": {
        "console": (BANNER, r'Found device "bme280@0", getting sensor data',
                    r"temp: 21\.3[67]\d*; press: 99\.88\d*; humidity: 45\.6[67]\d*"),
    },
    # samples/sensor/ds18b20/src/main.c:61: its yaml's "Temp: (.*)" as the
    # part's 21.375 °C.
    "zephyr/sensor/ds18b20": {"console": (BANNER, r"^Temp: 21\.375000$")},
    # samples/drivers/w1/scanner/src/main.c:16-31: its yaml's count line, as
    # the one part, which it reports by its family code first.
    "zephyr/drivers/w1/scanner": {
        "console": (BANNER, r"Device found; family: 0x28, serial: 0x[0-9a-f]+",
                    r"Number of devices found on bus: 1$"),
    },
    # samples/sensor/dht_polling: its yaml's line, which any reading
    # matches, as the BME280's 21.37 °C and 45.67 %RH.
    "zephyr/sensor/dht_polling": {
        "console": (BANNER, r"bme280@76: temp is 21\.3[67]\d* °C humidity is 45\.6[67]\d* %RH"),
    },
    # samples/sensor/thermometer: its yaml's lines, then the part's 25.25 °C
    # as the sample prints it, %0.1lf.
    "zephyr/sensor/thermometer": {
        "console": (BANNER, r"Thermometer Example (.*)",
                    r"Temperature device is 0x[0-9|a-z]+, name is [a-z|0-9]+@[a-z|0-9]+",
                    r"Temperature is 25\.[23]°C"),
    },
    # samples/drivers/modem/at_client: its yaml's three lines, then an AT
    # typed on the console comes back from the modem part as OK. The part
    # boots as the sample starts and takes commands once it reports SMS
    # Ready, which the bridge carries to the console too.
    "zephyr/drivers/modem/at_client": {
        "console": (BANNER, r"Console UART pipe registered", r"Modem pipe initialized and opened",
                    r"Console <-> Modem communication established", r"^SMS Ready$", r"^OK$"),
        "input": ((r"^SMS Ready$", b"AT\r"),),
        "seconds": 20.0,
    },
    # samples/drivers/lora/receive/src/main.c:72-89: its yaml asks only for
    # the "Synchronous reception" line, after which the sample blocks in
    # lora_recv four times. The radio part puts a frame on air at each, and
    # each must come back with the part's signal (-73 dBm, 7 dB SNR).
    "zephyr/drivers/lora/receive": {
        "console": (BANNER, r"<inf> lora_receive: Synchronous reception",
                    *(r"<inf> lora_receive: LoRa RX RSSI: -73 dBm, SNR: 7 dB",) * 4,
                    r"<inf> lora_receive: Asynchronous reception"),
        "input": ((r"<inf> lora_receive: Synchronous reception", lora_frame(RX_FRAMES[0])),
                  *((r"<inf> lora_receive: LoRa RX RSSI", lora_frame(f)) for f in RX_FRAMES[1:])),
        "seconds": 20.0,
    },
    # samples/subsys/zbus/confirmed_channel and priority_boost publish on a
    # timer and their patterns span several rounds: on the carrier the last
    # comes after more than 10 s.
    "zephyr/subsys/zbus/confirmed_channel": {"seconds": 30.0},
    "zephyr/subsys/zbus/priority_boost": {"seconds": 30.0},
    # samples/subsys/kvss/nvs reboots itself, counting down each time, up to
    # CONFIG_NVS_SAMPLE_MAX_REBOOT (5) times (src/main.c:189-231): a second
    # banner is the sample's own.
    "zephyr/subsys/kvss/nvs": {"forbidden": FAULTS, "seconds": 30.0},
    # samples/subsys/kvss/zms runs CONFIG_MAX_ITERATIONS (300) rounds of
    # writes before its line; on the carrier about 14 a second.
    "zephyr/subsys/kvss/zms": {"seconds": 60.0},
    # samples/subsys/sensing/simple/src/main.c:23-85: its yaml's line comes
    # once the sensors are opened, before any data; a sample from a 3D
    # sensor must follow, through the data callback.
    "zephyr/subsys/sensing/simple": {
        "console": (BANNER, r"sensing subsystem run successfully",
                    r"<inf> main: .*Sensor:.* data:\(x:-?\d+, y:-?\d+, z:-?\d+\)"),
        "seconds": 15.0,
    },
    # samples/subsys/logging/syst sends printk through its MIPI SyS-T log
    # backend, the boot banner too, which then reaches the console encoded
    # ("SYS-T RAW DATA: ..."): only the yaml's own lines can be asked for.
    "zephyr/subsys/logging/syst": {"console": (r"SYS-T RAW DATA: ", r"SYST Sample Execution Completed")},
    # samples/subsys/task_wdt: the control thread gets stuck and the task
    # watchdog's callback resets the chip right after printing
    # (src/main.c:46-58), so the reset cuts the UART's transmit FIFO short: the
    # callback line ends early and "Resetting device..." never reaches the
    # console, which the yaml's last two patterns need. The callback line's
    # start, then the second boot, show the same run; the cut lands a few
    # bytes earlier on the Pico 2 ("thread" against the Pico 1's "thread: c").
    "zephyr/subsys/task_wdt": {
        "console": (BANNER, r"Task watchdog sample application\.", r"Control thread started\.",
                    r"Main thread still alive\.\.\.", r"Control thread getting stuck\.\.\.",
                    r"Task watchdog channel 1 callback, thread", BANNER),
        "forbidden": FAULTS, "seconds": 20.0},
}


def own(*console: str, source: str, **fields) -> Spec:
    fields = {"seconds": SECONDS, "forbidden": FORBIDDEN, **fields}
    fields = {"forbidden_from": BANNER_START, **fields}
    return Spec(console=(BANNER, *console), lines=True, source=source, **fields)


def banner(metadata: dict) -> tuple[str, ...]:
    # The banner pattern, for an image built with the banner on (a sample may
    # turn CONFIG_BOOT_BANNER off: basic/hash_map).
    return (BANNER,) if metadata.get("boot_banner", True) else ()


def banner_start(metadata: dict) -> str:
    # Where forbidden output is judged from, for an image with the banner.
    return BANNER_START if metadata.get("boot_banner", True) else ""


# The shell's prompt on the console UART (CONFIG_SHELL_PROMPT_UART's
# default, subsys/shell/backends/Kconfig.backends).
PROMPT = r"^uart:~\$ "


def shell_spec(metadata: dict, source: str, prompt: str = PROMPT) -> Spec:
    # As twister's Shell harness (scripts/pylib/shell-twister-harness): an
    # Enter until a prompt answers, then at each prompt the next command
    # with a line end, and a line of its response must match its `expected`
    # from the line's start. The Enter goes in at the first prompt seen,
    # which may be one from before the restart still in the bridge; the
    # commands start at the prompt after it.
    commands = metadata.get("shell_commands") or []
    return Spec(console=(*banner(metadata), prompt,
                         *(f"^(?:{c['expected']})" for c in commands if c.get("expected"))),
                input=((prompt, b"\n"), *((prompt, f"{c['command']}\n".encode()) for c in commands)),
                seconds=SECONDS, forbidden=FORBIDDEN, forbidden_from=banner_start(metadata), lines=True,
                source=f"{source}; samples/{metadata['sample']}/test_shell.yml")


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
    # samples/sensor/pressure_polling/src/main.c:44-61: prints the sensor's
    # temperature (%.2f) and pressure (%f, kPa) every second; build_only
    # upstream. The bmp388 part holds 23.450 °C and 98765 Pa
    # (test/zephyr_samples/parts.py), within its model's 1 Pa and 1 m°C. Its
    # printf lines and the driver's debug log share the console and cut into
    # each other mid-line, so the reading is matched anywhere in a line.
    "zephyr/sensor/pressure_polling": own(
        r"temp 23\.4[4-6] Cel, pressure 98\.76[4-6]\d* kPa",
        forbidden=(*FORBIDDEN, r"Error"),
        source="samples/sensor/pressure_polling/src/main.c"),
    # samples/sensor/pressure_interrupt/src/main.c:36-61: logs each
    # data-ready trigger's reading; build_only upstream. The bmp388 part
    # holds 23.450 °C and 98765 Pa and raises INT on GP15 at each
    # conversion: three readings show the trigger running. Its deferred log
    # renders the format's space before each %f as a NUL byte
    # ("temp\x0023.45"), so any one character stands there.
    "zephyr/sensor/pressure_interrupt": own(
        *(r"temp.23\.4[4-6] Cel, pressure.98\.76[4-6]\d* kPa",) * 3,
        forbidden=(*FORBIDDEN, r"failed", r"Error", r"cancelling"),
        source="samples/sensor/pressure_interrupt/src/main.c"),
    # samples/sensor/6dof_motion_drdy/src/main.c:95-110: prints the reading
    # each data-ready trigger fetched; build_only upstream. The icm42688
    # part holds accel (2048, -2048, 1024) at 16 g full scale, 2048 LSB/g,
    # gyro (164, -164, 8200) at 2000 dps, 16.4 LSB/dps, and temperature
    # 1325, raw / 132.48 + 25 °C (DS-000347 3.1, 14.7): 35.00 °C. Three
    # readings show the trigger running.
    "zephyr/sensor/6dof_motion_drdy": own(
        *(r"temp 35\.00 Cel\s+accel 9\.80\d* -9\.80\d* 4\.90\d* m/s/s\s+"
          r"gyro\s+0\.17\d* -0\.17\d* 8\.72\d* rad/s",) * 3,
        forbidden=(*FORBIDDEN, r"failed", r"Error", r"cancelling", r"Cannot configure"),
        source="samples/sensor/6dof_motion_drdy/src/main.c"),
    # samples/net/cellular_modem/src/main.c: 16 echoes to the test endpoint's
    # UDP 7780, 128 datagrams to 7781 and its count of them back, the modem
    # suspended and resumed, 16 echoes again; its yaml has no harness. The
    # a7672x part is that endpoint (devices/cellular/README.md). Its shell
    # shares the console, so a line may follow the shell's prompt.
    "zephyr/net/cellular_modem": own(
        r"Resolved to 203\.0\.113\.7$", r"Server received 128/128 packets$", r"Server dropped 0 packets$",
        r"L4 connected$", r"Successfully sent and received 16 of 16 packets$", r"Sample complete$",
        seconds=180.0,
        forbidden=(*FORBIDDEN, r"Failed", r"not connected in time", r"Invalid response", r"query failed"),
        source="samples/net/cellular_modem/src/main.c"),
    # samples/drivers/gnss/src/main.c:15-27: prints each fix the driver
    # publishes; its yaml has no harness. The u-blox UBX driver maps a 2D or
    # 3D fix to GNSS_FIX_STATUS_GNSS_FIX, 1 (drivers/gnss/u_blox/gnss_ubx_common.c,
    # include/zephyr/drivers/gnss.h).
    "zephyr/drivers/gnss": own(r"Got a fix \(type: 1\)", seconds=30.0, source="samples/drivers/gnss/src/main.c"),
    # Wave 1e: what each sensor sample prints of its part's values
    # (test/zephyr_samples/parts.py), within the registers' resolution.
    # samples/sensor/mpu6050/src/main.c:52-60: die temperature (%g), then
    # acceleration (m/s/s) and rotation (rad/s), %f: 250, -500, 1125 mg and
    # 10, -2, 3 deg/s at the default full scales. The part's 26.53 °C reads
    # as 26: Zephyr's driver adds 36.0 °C where the register map's formula
    # has 36.53 (drivers/sensor/tdk/mpu6050/mpu6050.c:56).
    "zephyr/sensor/mpu6050": own(
        r"^\[[0-9:.]+\]:26 Cel$", r"^  accel 2\.45\d* -4\.90\d* 11\.03\d* m/s/s$",
        r"^  gyro  0\.174\d* -0\.034\d* 0\.052\d* rad/s$",
        source="samples/sensor/mpu6050/src/main.c; harness_config none"),
    # samples/sensor/lis2dh/src/main.c:36-54: acceleration, %f m/s/s, of
    # the part's 252, -500, 1020 mg. Zephyr's LIS2DH driver scales a digit
    # as 1/1024 g at +-2 g (ACCEL_SCALE(1600), drivers/sensor/st/lis2dh/
    # lis2dh.c:20-28), not the datasheet's typical 1 mg, so it reads 2.3 %
    # low, on silicon as here: 2.4133, -4.7884, 9.7679.
    "zephyr/sensor/lis2dh": own(
        r"x 2\.41\d* , y -4\.78\d* , z 9\.76\d*",
        forbidden=(*FORBIDDEN, r"ERROR"),
        source="samples/sensor/lis2dh/src/main.c; harness_config none"),
    # samples/sensor/ina219/src/main.c:30-37: the part's 5.124 V bus, a
    # whole number of its 4 mV steps, and 123.4 mA, of the overlay's 10 uA
    # steps: both exact, but for the driver's double arithmetic, which
    # leaves 1281 x 0.004 V at 5.123999.
    "zephyr/sensor/ina219": own(
        r"^Bus: 5\.12(3999|4000) \[V\] -- Power: \S+ \[W\] -- Current: 0\.123400 \[A\]$",
        forbidden=(*FORBIDDEN, r"Could not fetch"),
        source="samples/sensor/ina219/src/main.c; harness_config none"),
    # samples/basic/button/src/main.c:20-41: each sw0 event, with its key
    # code (INPUT_KEY_0, 11), pressed and released, as the harness presses.
    "zephyr/basic/button": own(r"^Press the button$", r"^Button 11 pressed at \d+$", r"^Button 11 released at \d+$",
                               r"^Button 11 pressed at \d+$",
                               source="samples/basic/button/src/main.c"),
    # samples/drivers/gpio/button_interrupt/src/main.c:36-68: a line per
    # press, from sw0's interrupt; its yaml's harness is "button".
    "zephyr/drivers/gpio/button_interrupt": own(
        r"^Set up button at \S+ pin 15$", r"^Button pressed at \d+$", r"^Button pressed at \d+$",
        forbidden=(*FORBIDDEN, r"^Error"),
        source="samples/drivers/gpio/button_interrupt/src/main.c"),
    # samples/drivers/ht16k33/src/main.c:26-36: lights the LEDs one by one
    # (12.8 s), then blinks them; the part's display RAM is checked too
    # (test/zephyr_samples/parts.py).
    "zephyr/drivers/ht16k33": own(r"<inf> main: Iterating through all LEDs, turning them on one-by-one",
                                  r"<inf> main: Blinking LEDs with a period of 500 ms", seconds=20.0,
                                  forbidden=(*FORBIDDEN, r"<err>"),
                                  source="samples/drivers/ht16k33/src/main.c"),
    # samples/drivers/display/src/display.c:276-457: its builtin test's yaml
    # has a fixture and no patterns; the log names the display, then starts.
    "zephyr/drivers/display": own(r"<inf> sample: Display sample for ssd1306@3c", r"<inf> sample: Display starts",
                                  forbidden=(*FORBIDDEN, r"<err>"),
                                  source="samples/drivers/display/src/display.c"),
    # samples/drivers/uart/passthrough/src/main.c:136-140: names both UARTs,
    # then copies each one's input to the other; typed text goes out uart1,
    # through the harness's loop, and back to the console.
    "zephyr/drivers/uart/passthrough": own(
        r"^Console Device: 0x[0-9a-f]+$", r"^Other Device:   0x[0-9a-f]+$", r"metalclaw",
        input=((r"^Other Device:   0x[0-9a-f]+$", b"metalclaw"),),
        forbidden=(*FORBIDDEN, r"Rx Error", r"Rx Overflow", r"Tx Error"),
        source="samples/drivers/uart/passthrough/src/main.c"),
    # samples/drivers/led/led_strip/src/main.c:48-67: finds the strip, then
    # shows its pattern; the part's frames are checked too.
    "zephyr/drivers/led/led_strip": own(r"<inf> main: Found LED strip device ws2812",
                                        r"<inf> main: Displaying pattern on strip",
                                        forbidden=(*FORBIDDEN, r"<err>"),
                                        source="samples/drivers/led/led_strip/src/main.c"),
    # samples/basic/rgb_led and servo_motor: a banner, then PWM changes that
    # print only on an error.
    "zephyr/basic/rgb_led": own(r"^PWM-based RGB LED control$", forbidden=(*FORBIDDEN, r"Error"), seconds=3.0,
                                source="samples/basic/rgb_led/src/main.c"),
    "zephyr/basic/servo_motor": own(r"^Servomotor control$", forbidden=(*FORBIDDEN, r"Error"), seconds=3.0,
                                    source="samples/basic/servo_motor/src/main.c"),
    # samples/subsys/fs/littlefs/src/main.c:68-102: mounts the storage
    # partition and counts boots in a file. The partition is erased at each
    # restart (the image's erased storage block), so the sample formats it,
    # finds no count and writes 1.
    "zephyr/subsys/fs/littlefs": own(r"/boot_count read count:0 \(bytes: 0\)$",
                                     r"/boot_count write new boot count 1: \[wr:1\]$",
                                     forbidden=(*FORBIDDEN, r"FAIL"), seconds=30.0,
                                     source="samples/subsys/fs/littlefs/src/main.c"),
    # samples/sensor/jc42/src/main.c:132-155: its yaml's harness is
    # "sensor", with no patterns; it prints the part's temperature every
    # 2 s, after its uptime. The mcp9808 part reads 25.25 C (parts.py).
    "zephyr/sensor/jc42": own(r"^\d+:\d{2}:\d{2}\.\d{3}: 25\.25 C$",
                              forbidden=(*FORBIDDEN, r"error", r"failed", r"not ready", r"not found"),
                              source="samples/sensor/jc42/src/main.c"),
    # samples/subsys/logging/logger's user-mode test: its yaml's harness is
    # keyboard, with no patterns. src/main.c:245-300 runs the logger demo
    # from the kernel thread and then from a user thread, each under its
    # own banner, through the same showcases the basic test's yaml names.
    "zephyr/subsys/logging/logger@usermode": own(
        r"---=< RUNNING LOGGER DEMO FROM USER THREAD >=---", r"Instance level logging showcase",
        r"Severity levels showcase", r"Logs from external logging system showcase", seconds=30.0,
        source="samples/subsys/logging/logger/src/main.c"),
    # samples/subsys/fs/format/src/main.c:45-52: formats its file system
    # (fs_mkfs) and logs the result, through the minimal logger.
    **{t: own(r"Format successful$", forbidden=(*FORBIDDEN, r"Format failed"),
              source="samples/subsys/fs/format/src/main.c")
       for t in ("zephyr/subsys/fs/format", "zephyr/subsys/fs/format@fat_fs")},
    # samples/subsys/display/cfb/src/main.c:35-60: initialises the 128x64
    # panel's framebuffer, lists its fonts and its geometry; the part's
    # GDDRAM is checked too. Its printf lines come straight out, while the
    # banner goes through the deferred logger and lands after them, so the
    # lines match in any order.
    "zephyr/subsys/display/cfb": own(r"^Initialized ssd1306@3c$", r"^font width \d+, font height \d+$",
                                     r"^x_res 128, y_res 64, ppt \d+, rows \d+, cols \d+$", ordered=False,
                                     forbidden=(*FORBIDDEN, r"[Ff]ailed", r"not ready"),
                                     source="samples/subsys/display/cfb/src/main.c"),
    # samples/subsys/display/cfb_shell: the cfb shell commands
    # (subsys/fb/cfb_shell.c:430-505): initialise the framebuffer, read its
    # width, print text; its yaml (harness display) has no commands. The
    # part's GDDRAM must then hold something. The replies show the run; the
    # banner is not asked for, as a stale prompt's escapes from before the
    # restart can run into it.
    "zephyr/subsys/display/cfb_shell": shell_spec({"sample": "subsys/display/cfb_shell", "boot_banner": False,
                                                   "shell_commands": [
        {"command": "cfb init", "expected": r"Framebuffer initialized: ssd1306@3c"},
        {"command": "cfb get_param width", "expected": r"param: width=128"},
        {"command": "cfb print 0 0 metalclaw"}]}, "subsys/fb/cfb_shell.c"),
    # samples/drivers/auxdisplay_digits/src/main.c: logs only failures; its
    # pins judge it (test/zephyr_samples/pin_specs.py).
    "zephyr/drivers/auxdisplay_digits": own(forbidden=(*FORBIDDEN, r"<err>"), seconds=5.0,
                                            source="samples/drivers/auxdisplay_digits/src/main.c"),
    # samples/subsys/shell/shell_module/src/main.c: the demo commands
    # (:84-97, 225-232, 155-201). All three configurations turn the banner
    # off (prj.conf, prj_minimal.conf, prj_getopt.conf); robot is the
    # yaml's robot suite, shell_module.robot: `version` at the prompt.
    "zephyr/subsys/shell/shell_module@minimal": shell_spec(
        {"sample": "subsys/shell/shell_module", "boot_banner": False, "shell_commands": [
            {"command": "demo ping", "expected": r"pong"},
            {"command": "version", "expected": r"Zephyr version \d+\.\d+\.\d+"}]},
        "samples/subsys/shell/shell_module/src/main.c"),
    "zephyr/subsys/shell/shell_module@getopt": shell_spec(
        {"sample": "subsys/shell/shell_module", "boot_banner": False, "shell_commands": [
            {"command": "demo getopt -b", "expected": r"aflag = 0, bflag = 1"},
            {"command": "demo getopt -a -c", "expected": r"Option -c requires an argument\."}]},
        "samples/subsys/shell/shell_module/src/main.c"),
    "zephyr/subsys/shell/shell_module@robot": shell_spec(
        {"sample": "subsys/shell/shell_module", "boot_banner": False, "shell_commands": [
            {"command": "version", "expected": r"Zephyr version"}]},
        "samples/subsys/shell/shell_module/shell_module.robot"),
    # shell_module's login test (prj_login.conf: no banner, an obscured
    # login: prompt whose root command is login; src/login_cmd.c:28-48).
    # As at any shell, an Enter first at a login: prompt, which may be one
    # from before the restart: it ends stray input there, and the password
    # goes in at the prompt after it. The boot's hint is not needed. Then
    # the welcome, and a command at the ordinary prompt. The Enter may
    # submit stray input as a password, which the sample refuses
    # ("Incorrect password!"); the password is typed once, so the welcome
    # shows it was taken.
    "zephyr/subsys/shell/shell_module@login": Spec(
        console=(r"^login: ", r"^Shell Login Demo$", PROMPT, r"^Zephyr version \d"),
        input=((r"^login: ", b"\n"), (r"^login: ", b"zephyr\n"), (PROMPT, b"kernel version\n")),
        seconds=SECONDS, forbidden=FORBIDDEN, lines=True,
        source="samples/subsys/shell/shell_module/src/login_cmd.c"),
    # samples/subsys/shell/fs: its yaml's harness is keyboard, with no
    # commands. The fs shell (subsys/fs/shell.c:877-905, 542-600, 180-210,
    # 425-456) mounts littlefs on the erased storage partition, writes a
    # file of the bytes given, lists it, and dumps it as hex and text.
    "zephyr/subsys/shell/fs": shell_spec({"sample": "subsys/shell/fs", "shell_commands": [
        {"command": "fs mount littlefs /lfs", "expected": r"Successfully mounted littlefs at /lfs"},
        {"command": "fs write /lfs/mc.txt 6d 65 74 61 6c 63 6c 61 77"},
        {"command": "fs ls /lfs", "expected": r".*\bmc\.txt$"},
        {"command": "fs read /lfs/mc.txt", "expected": r"00000000  6D 65 74 61 6C 63 6C 61 77 .*metalclaw$"}]},
        "subsys/fs/shell.c"),
    # samples/subsys/display/lvgl: its yaml's harness is "none"; it logs
    # only failures (LVGL's own warnings too, CONFIG_LV_USE_LOG). The panel's
    # GDDRAM judges it (test/zephyr_samples/parts.py).
    # samples/modules/lvgl/screen_transparency/src/main.c: its yaml's
    # harness is none; it logs only failures. The matrix's LEDs judge it
    # (parts.py).
    "zephyr/modules/lvgl/screen_transparency": own(forbidden=(*FORBIDDEN, r"<err>", r"\[Error\]"), seconds=5.0,
                                                   source="samples/modules/lvgl/screen_transparency/src/main.c"),
    # samples/modules/lvgl/multi_display/src/main.c: its yaml has no
    # patterns; it logs only failures, and runs a demo on the first panel
    # and another on the second. Both panels' GDDRAM judge it (parts.py).
    "zephyr/modules/lvgl/multi_display": own(forbidden=(*FORBIDDEN, r"<err>", r"\[Error\]"), seconds=5.0,
                                             source="samples/modules/lvgl/multi_display/src/main.c"),
    # samples/modules/lvgl/accelerometer_chart/src/main.c: its yaml's
    # harness is none; it logs only failures, and charts accel0's readings.
    # The panel's GDDRAM judges it (parts.py).
    "zephyr/modules/lvgl/accelerometer_chart": own(forbidden=(*FORBIDDEN, r"<err>", r"\[Error\]"), seconds=5.0,
                                                   source="samples/modules/lvgl/accelerometer_chart/src/main.c"),
    "zephyr/subsys/display/lvgl": own(forbidden=(*FORBIDDEN, r"<err>", r"\[Error\]"), seconds=5.0,
                                      source="samples/subsys/display/lvgl/src/main.c"),
    # samples/subsys/display/cfb_custom_font: prints nothing unless it
    # fails; the part's GDDRAM judges it.
    "zephyr/subsys/display/cfb_custom_font": own(forbidden=(*FORBIDDEN, r"[Ff]ailed", r"not ready", r"Could not"),
                                                 seconds=3.0,
                                                 source="samples/subsys/display/cfb_custom_font/src/main.c"),
    # samples/drivers/adc: readings of the harness's levels
    # (test/zephyr_samples/parts.py): about 825 mV on input 0 and 2475 mV
    # on input 1 through the carrier's RC filters, within 65 mV; on a bench
    # without them the inputs sit low and high.
    "zephyr/drivers/adc/adc_dt": own(
        r"^ADC reading\[\d+\]:$", r"^- adc@[0-9a-f]+, channel 0: \d+ = (7[6-9]\d|8[0-8]\d) mV$",
        analog_pins=(26,), without_analog={"console": (BANNER, r"^ADC reading\[\d+\]:$",
                                                       r"^- adc@[0-9a-f]+, channel 0: \d+ = [0-5]?\d mV$")},
        source="samples/drivers/adc/adc_dt/src/main.c"),
    "zephyr/drivers/adc/adc_sequence": own(
        r"^ADC sequence reading \[\d+\]:$", r"^- adc@[0-9a-f]+, channel 0, \d+ sequence samples:$",
        r"^- - \d+ = (7[6-9]\d|8[0-8]\d)mV$", r"^- adc@[0-9a-f]+, channel 1, \d+ sequence samples:$",
        r"^- - \d+ = (24[1-9]\d|25[0-3]\d)mV$",
        analog_pins=(26, 27),
        without_analog={"console": (BANNER, r"^ADC sequence reading \[\d+\]:$",
                                    r"^- adc@[0-9a-f]+, channel 0, \d+ sequence samples:$", r"^- - \d+ = [0-5]?\dmV$",
                                    r"^- adc@[0-9a-f]+, channel 1, \d+ sequence samples:$",
                                    r"^- - \d+ = (32[4-9]\d|33\d\d)mV$")},
        source="samples/drivers/adc/adc_sequence/src/main.c"),
    # samples/sensor/sm351lt/src/main.c:20-87: polls every 2 s and prints
    # the switch's state; the harness's wave shows both.
    "zephyr/sensor/sm351lt": own(r"^Polling at 0\.5 Hz$", r"^#\d+ @ \d+ ms: [01]$", r"^#\d+ @ \d+ ms: 1$",
                                 r"^#\d+ @ \d+ ms: 0$", ordered=False, seconds=15.0,
                                 forbidden=(*FORBIDDEN, r"ERROR"), source="samples/sensor/sm351lt/src/main.c"),
    # samples/subsys/input/input_dump: CONFIG_INPUT_EVENT_DUMP logs each
    # event (subsys/input/input_utils.c:62): the button's key (EV_KEY 1,
    # INPUT_KEY_0 11) pressed and released, and the wheel's steps (EV_REL 2,
    # INPUT_REL_WHEEL 8), in whatever order they come.
    "zephyr/subsys/input/input_dump": own(
        r"input event: dev=buttons .*type= 1 code= 11 value=1", r"input event: dev=buttons .*type= 1 code= 11 value=0",
        r"input event: dev=qdec .*type= 2 code=  8 value=-?[1-9]", ordered=False,
        source="samples/subsys/input/input_dump; subsys/input/input_utils.c"),
    # samples/sensor/sensor_shell (harness keyboard): `sensor get` on the
    # INA237 prints each channel, the value in parentheses after its
    # timestamp (drivers/sensor/sensor_shell.c:528-531): the part's 3.3 V
    # bus and 200 mA, in the overlay's 500 uA current steps.
    "zephyr/sensor/sensor_shell": shell_spec(
        {"sample": "sensor/sensor_shell", "boot_banner": False, "shell_commands": [
            {"command": "sensor get ina237@40",
             "expected": r"channel type=\d+\(voltage\) index=0 shift=\d+ num_samples=1 value=\d+ns \(3\.(29|30)\d*\)"},
            {"command": "sensor get ina237@40 current",
             "expected": r"channel type=\d+\(current\) index=0 shift=\d+ num_samples=1 value=\d+ns \(0\.(199|200)\d*\)"}]},
        "drivers/sensor/sensor_shell.c"),
    # samples/drivers/stepper/generic/src/main.c:69-107: each button press
    # moves to the next mode and logs it, first enabling the driver.
    "zephyr/drivers/stepper/generic": own(r"<inf> stepper_generic: Starting generic stepper sample",
                                          r"<inf> stepper_generic: mode: enable",
                                          r"<inf> stepper_generic: mode: ping pong relative",
                                          forbidden=(*FORBIDDEN, r"<err>"),
                                          source="samples/drivers/stepper/generic/src/main.c"),
    # samples/sensor/grove_temperature/src/main.c:78 and light_polling's
    # src/main.c:35 print what the Grove drivers make of ADC input 0
    # (drivers/sensor/seeed/grove): at half of full scale the thermistor
    # reads 25 °C, and the light sensor 10000 / (10 * 15)^(4/3) = 12.6 lux,
    # within the carrier's ripple. Their yaml harness is "grove". On a bench
    # without RC filters the input sits high: no such reading.
    "zephyr/sensor/grove_temperature": own(r"^Temperature: 2[3-7]\.\d+ C$", analog_pins=(26,),
                                           without_analog={"console": (BANNER, r"^Temperature: -?\d+\.\d+ C$")},
                                           source="samples/sensor/grove_temperature/src/main.c"),
    "zephyr/sensor/light_polling": own(r"^lux: 1[12]\.\d+$", analog_pins=(26,),
                                       without_analog={"console": (BANNER, r"^lux: \d+\.\d+$")},
                                       source="samples/sensor/light_polling/src/main.c"),
    # samples/drivers/fuel_gauge/src/main.c:112-330: finds the gauge, lists
    # what it supports (logging an error for each string property the
    # composite gauge lacks, -ENOSYS), then polls it: the cell voltage the
    # INA219 gives it, the part's 3.7 V, a whole number of 4 mV steps.
    "zephyr/drivers/fuel_gauge": own(r"<inf> app: Found device \"fuel-gauge\"",
                                     r"<inf> app: Fuel gauge data: Charge: \d+%, Voltage: 3700mV$",
                                     source="samples/drivers/fuel_gauge/src/main.c"),
    # samples/drivers/auxdisplay/src/main.c: writes its greeting and logs
    # only a failure; the part's DDRAM judges it.
    # samples/drivers/biometrics/fingerprint/src/main.c: enrolls IDs 1 and
    # 2, lists them, verifies ID 1, identifies (the first page holding the
    # finger: ID 1), deletes ID 1 and lists again. The zfm20 part holds
    # finger 1 and reports score 157. Its yaml wants "Test complete". The
    # driver's "initialized" line is not judged: it never showed after the
    # banner on either fixture (its init runs at POST_KERNEL, before it).
    "zephyr/drivers/biometrics/fingerprint": own(
        *(r"<inf> main: Enrollment complete",) * 2,
        r"<inf> main: Found 2 stored template\(s\):",
        r"<inf> main:   - Template ID: 1", r"<inf> main:   - Template ID: 2",
        r"<inf> main: Match! Confidence: 157, Quality: 0",
        r"<inf> main: Matched ID 1, confidence 157, quality 0",
        r"<inf> main: Found 1 stored template\(s\):",
        r"<inf> main:   - Template ID: 2",
        r"<inf> main: Test complete",
        forbidden=(*FORBIDDEN, r"<err>", r"<wrn>"),
        source="samples/drivers/biometrics/fingerprint/src/main.c"),
    # samples/drivers/counter/maxim_ds3231/src/main.c: build_only upstream.
    # main() reads the part's power-on oscillator fault, sets the clock to
    # the next hour on the square wave, synchronizes, then sets alarm 1 five
    # seconds on; its handler sets a counter alarm ten seconds after that,
    # and alarm 2 fires at each minute's 00 seconds (min_alarm_handler).
    # set_alarm returns a non-negative value on success (the sample tests
    # rc >= 0). The minute line's adjustment prints a negative one as -1
    # and the nanoseconds above it; its error compares the part's seconds
    # with the DUT's clock since the sync, which a false sync edge would
    # put a thousand ppm and more off.
    "zephyr/drivers/counter/maxim_ds3231": own(
        r"^DS3231 has experienced an oscillator fault$",
        r"^Synchronize final: 0 0 in \d+ ms$",
        r"^Synchronize complete in \d+ ms: 0 0$",
        r"^Set sec alarm [0-9a-f]+ at \d+ ~ .*: \d+$",
        r"^Sec signaled at \d+ ms, param 0x[0-9a-f]+, delay \d+; set 0$",
        r"^Counter callback at \d+ ms, id 0, ticks \d+, ud 0x[0-9a-f]+$",
        r"^\d{4}-\d\d-\d\d \d\d:\d\d:00 \w{3} \d{3}: adj -?[01]\.\d{9}, "
        r"uptime \d+:\d\d:\d\d\.\d{3}, clk err -?\d{1,3} ppm$",
        forbidden=(*FORBIDDEN, r"not ready", r"failed"), seconds=80.0,
        source="samples/drivers/counter/maxim_ds3231/src/main.c"),
    # samples/sensor/hts221/src/main.c:33-40: an observation every 2 s. Its
    # yaml takes any number; the part holds 23.5 C and 47.5 %RH.
    "zephyr/sensor/hts221": own(
        *(r"^Observation:\d+$", r"^Temperature:23\.5 C$", r"^Relative Humidity:47\.5%$") * 2,
        forbidden=(*FORBIDDEN, r"error", r"Cannot", r"not ready"), seconds=8.0,
        source="samples/sensor/hts221/src/main.c"),
    # samples/sensor/max6675/src/main.c:45: a reading a second. Its yaml
    # takes any number; the part holds 123.25 C, a whole count of 0.25 C.
    "zephyr/sensor/max6675": own(*(r"^Temperature: 123\.25 C$",) * 2,
                                 forbidden=(*FORBIDDEN, r"Could not", r"not ready"), seconds=5.0,
                                 source="samples/sensor/max6675/src/main.c"),
    # samples/sensor/magn_polling/src/main.c:36 and magn_trig/src/main.c:54:
    # the field in gauss, Zephyr's AK8963 driver scaling counts by 1499 uG
    # at a sensitivity adjustment of 128 (drivers/sensor/tdk/mpu9250/
    # ak8963.c): 200, -100 and 300 counts. magn_polling's yaml harness is
    # "sensor"; magn_trig is judged by its yaml.
    "zephyr/sensor/magn_polling": own(*(r"\( x y z \) = \( 0\.299800  -0\.149900  0\.449700 \)",) * 3,
                                      forbidden=(*FORBIDDEN, r"failed", r"not ready"), seconds=6.0,
                                      source="samples/sensor/magn_polling/src/main.c"),
    # The same with an LSM303DLHC's magnetometer: 275, -110 and 490 counts
    # at 1100 LSB/gauss for X and Y and 980 for Z (Doc ID 018771 Table 75,
    # the driver's default range).
    "zephyr/sensor/magn_polling@lsm303dlhc": own(
        *(r"\( x y z \) = \( 0\.250000  -0\.100000  0\.500000 \)",) * 3,
        forbidden=(*FORBIDDEN, r"failed", r"not ready", r"not available"), seconds=6.0,
        source="samples/sensor/magn_polling/src/main.c"),
    # samples/sensor/lps22hb/src/main.c:34-40: each observation's pressure
    # and temperature. Its yaml asks for a "Relative Humidity" line the
    # source never prints, so these are the source's lines; the part holds
    # 100 kPa and 23.5 C.
    "zephyr/sensor/lps22hb": own(*(r"^Pressure:100\.0 kPa$", r"^Temperature:23\.5 C$") * 2,
                                 forbidden=(*FORBIDDEN, r"error", r"Cannot read"), seconds=6.0,
                                 source="samples/sensor/lps22hb/src/main.c"),
    # samples/sensor/veml6031/src/main.c:56 and veml6046/src/main.c:75: one
    # line per integration time, gain and divider setting it sweeps, with
    # the raw counts; their yaml's harness is "sensor". The parts hold 1000
    # counts on every channel; three such lines show the sweep running.
    "zephyr/sensor/veml6031": own(*(r"ALS \(raw\):\s+1000 IR \(raw\):\s+1000 ",) * 3,
                                  forbidden=(*FORBIDDEN, r"ERROR", r"Failed"), seconds=8.0,
                                  source="samples/sensor/veml6031/src/main.c"),
    "zephyr/sensor/veml6046": own(*(r"Red:\s+\d+ lx \(\s*1000\) green:\s+\d+ lx \(\s*1000\) "
                                    r"blue:\s+\d+ lx \(\s*1000\) IR:\s+\d+ lx \(\s*1000\)",) * 3,
                                  forbidden=(*FORBIDDEN, r"ERROR", r"Failed"), seconds=8.0,
                                  source="samples/sensor/veml6046/src/main.c"),
    # samples/sensor/sht3xd/src/main.c:95 and sht4x/src/main.c:70: a reading
    # every two seconds; sht4x is build_only upstream, and sht3xd's yaml
    # has no harness. The parts hold 23.5 C and 45 %RH, within their 16-bit
    # words' 3 mC and 2 m%RH.
    "zephyr/sensor/sht3xd": own(*(r"SHT3XD: 23\.50 Cel ; 45\.00 %RH",) * 2,
                                forbidden=(*FORBIDDEN, r"failed", r"not ready"), seconds=8.0,
                                source="samples/sensor/sht3xd/src/main.c"),
    "zephyr/sensor/sht4x": own(*(r"SHT4X: 23\.50 Temp\. \[C\] ; 45\.00 RH \[%\]",) * 2,
                               forbidden=(*FORBIDDEN, r"Failed", r"not ready"), seconds=8.0,
                               source="samples/sensor/sht4x/src/main.c"),
    # samples/drivers/misc/grove_display/src/main.c: prints only when the
    # LCD is not ready; its yaml's harness is "grove", a person looking.
    # The parts judge it.
    "zephyr/drivers/misc/grove_display": own(forbidden=(*FORBIDDEN, r"not ready"), seconds=3.0,
                                             source="samples/drivers/misc/grove_display/src/main.c"),
    # samples/drivers/led/pca9633/src/main.c:27-40: logs the device found
    # and the test starting, then errors only; build_only upstream.
    "zephyr/drivers/led/pca9633": own(r"Found LED device", r"Testing leds", forbidden=(*FORBIDDEN, r"<err>"),
                                      seconds=5.0, source="samples/drivers/led/pca9633/src/main.c"),
    "zephyr/drivers/auxdisplay": own(forbidden=(*FORBIDDEN, r"<err>"), seconds=3.0,
                                     source="samples/drivers/auxdisplay/src/main.c"),
    # Typed console samples, whose yaml harness is "keyboard": a person types.
    # samples/subsys/console/echo/src/main.c:8-26: echoes each character,
    # adding a line feed after a carriage return.
    "zephyr/subsys/console/echo": own(
        r"^Start typing characters to see them echoed back$", r"^metalclaw$",
        input=((r"^Start typing characters to see them echoed back$", b"metalclaw\r"),),
        source="samples/subsys/console/echo/src/main.c"),
    # samples/subsys/console/getchar/src/main.c:13-21: prints each
    # character's code and the character.
    "zephyr/subsys/console/getchar": own(
        r"^Start typing characters to see their hex codes printed$", r"^char: \[0x61\] a$", r"^char: \[0x62\] b$",
        input=((r"^Start typing characters to see their hex codes printed$", b"ab"),),
        source="samples/subsys/console/getchar/src/main.c"),
    # samples/subsys/console/getline/src/main.c:14-23: prints each line it
    # reads and the line's last character.
    "zephyr/subsys/console/getline": own(
        r"^Enter a line$", r"^line: hi$", r"^last char was: 0x69$",
        input=((r"^Enter a line$", b"hi\r"),),
        source="samples/subsys/console/getline/src/main.c"),
    # samples/drivers/uart/echo_bot/src/main.c:97-105: greets, then echoes
    # each line that ends in a carriage return or line feed.
    "zephyr/drivers/uart/echo_bot": own(
        r"^Hello! I'm your echo bot\.$", r"^Tell me something and press enter:$", r"^Echo: metalclaw$",
        input=((r"^Tell me something and press enter:$", b"metalclaw\r"),),
        source="samples/drivers/uart/echo_bot/src/main.c"),
    # samples/arch/mpu/mpu_test: its test_shell.yml reads words at 0x1 and
    # 0x2 with `mpu mtest`, unaligned loads that an ARMv6-M core (the
    # RP2040's) faults on; the Pico 1 build reads the boot ROM's aligned
    # words at 0x0 and 0x4 instead (src/main.c:124-139).
    "zephyr/arch/mpu/mpu_test": replace(
        shell_spec({"sample": "arch/mpu/mpu_test", "shell_commands": [
            {"command": "mpu mtest 1", "expected": "The value is: 0x.*"},
            {"command": "mpu mtest 2", "expected": "The value is: 0x.*"}]},
            "samples/arch/mpu/mpu_test/test_shell.yml"),
        per_board={"pico": {"input": shell_spec({"sample": "arch/mpu/mpu_test", "shell_commands": [
            {"command": "mpu mtest 0"}, {"command": "mpu mtest 4"}]}, "").input}}),
    # samples/boards/raspberrypi/rpi_pico/uart_pio/src/main.c: echoes what
    # each of two PIO UARTs receives; the first takes GP0/GP1, the console's
    # pins, before the boot banner, so nothing comes before the echo. Text
    # typed once the chip is up must come back. No sample.yaml.
    "zephyr/boards/raspberrypi/rpi_pico/uart_pio": Spec(
        console=(r"metalclaw",), input=((r"", "import time\ntime.sleep_ms(500)\n"), (r"", b"metalclaw")),
        seconds=5.0, forbidden=FAULTS, source="samples/boards/raspberrypi/rpi_pico/uart_pio/src/main.c"),
    # samples/subsys/shell/devmem_load (harness keyboard): `devmem load`
    # takes hex text until Ctrl-X Ctrl-Q and reports the bytes read
    # (subsys/shell/modules/devmem_service.c:166-206); `devmem <addr> 32`
    # then reads the first word back: without -e each 8 hex digits are one
    # word's value, so 0xdeadbeef. 0x20030000 is SRAM the
    # sample leaves alone. No banner (its prj.conf).
    "zephyr/subsys/shell/devmem_load": Spec(
        console=(PROMPT, r"^press ctrl-x ctrl-q to escape$", r"^Number of bytes read: 8$",
                 r"^Read value 0xdeadbeef$"),
        input=((PROMPT, b"\n"), (PROMPT, b"devmem load 0x20030000\n"),
               (r"^press ctrl-x ctrl-q to escape$", b"deadbeef0badcafe\x18\x11"),
               (PROMPT, b"devmem 0x20030000 32\n")),
        seconds=SECONDS, forbidden=FORBIDDEN, lines=True,
        source="samples/subsys/shell/devmem_load; subsys/shell/modules/devmem_service.c"),
    # samples/subsys/testsuite/pytest/basic: a ztest suite whose one test
    # prints Hello world; its yaml's host pytest script checks only the
    # host side's options, so the suite's own summary judges it.
    "zephyr/subsys/testsuite/pytest/basic": own(r"Hello world$", r"^PROJECT EXECUTION SUCCESSFUL$",
                                                forbidden=(*FORBIDDEN, r"PROJECT EXECUTION FAILED"),
                                                source="samples/subsys/testsuite/pytest/basic/src/main.c"),
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
                    forbidden=(*FORBIDDEN, r"PROJECT EXECUTION FAILED"), forbidden_from=banner_start(metadata),
                    lines=True, source=source)
        return replace(spec, **OVERRIDES.get(metadata["target"], {}))
    if metadata.get("harness") == "shell":
        return replace(shell_spec(metadata, source), **OVERRIDES.get(metadata["target"], {}))
    if metadata.get("harness") != "console":
        return None
    config = metadata["harness_config"]
    regex = tuple(config["regex"])
    if config["type"] == "one_line":
        patterns, ordered = regex[:1], True
    else:
        patterns, ordered = regex, config.get("ordered", True)
    spec = Spec(console=(*banner(metadata), *patterns), seconds=SECONDS, forbidden=FORBIDDEN,
                forbidden_from=banner_start(metadata), lines=True, ordered=ordered, source=source)
    return replace(spec, **OVERRIDES.get(metadata["target"], {}))
