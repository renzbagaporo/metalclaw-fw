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
    # samples/drivers/watchdog: feeds the watchdog five times, then lets it
    # reset the chip; its yaml expects the second boot.
    "zephyr/drivers/watchdog": {"forbidden": FAULTS, "seconds": 20.0},
    # samples/sensor/accel_polling: its yaml's line, which a reading in any
    # range matches, as the part's 252, -500, 1020 mg in m/s^2 (%12.6f).
    "zephyr/sensor/accel_polling": {
        "console": (BANNER, r"^\s*lis2dh@18 \[m/s\^2\]:    \(\s*2\.4[67]\d*,\s*-4\.9[01]\d*,\s*10\.0[0-2]\d*\)$"),
    },
    # samples/sensor/bme280: its yaml's line, which any reading matches, as
    # the part's 21.37 °C, 99.887 kPa and 45.67 %RH (src/main.c:95-98).
    "zephyr/sensor/bme280": {
        "console": (BANNER, r'Found device "bme280@0", getting sensor data',
                    r"temp: 21\.3[67]\d*; press: 99\.88[6-8]\d*; humidity: 45\.6[67]\d*"),
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
    # samples/sensor/thermometer: its yaml's lines, then the part's 25.25 °C
    # as the sample prints it, %0.1lf.
    "zephyr/sensor/thermometer": {
        "console": (BANNER, r"Thermometer Example (.*)",
                    r"Temperature device is 0x[0-9|a-z]+, name is [a-z|0-9]+@[a-z|0-9]+",
                    r"Temperature is 25\.[23]°C"),
    },
    # samples/drivers/modem/at_client: its yaml's three lines, then an AT
    # typed on the console comes back from the modem part as OK.
    "zephyr/drivers/modem/at_client": {
        "console": (BANNER, r"Console UART pipe registered", r"Modem pipe initialized and opened",
                    r"Console <-> Modem communication established", r"^OK$"),
        "input": ((r"Ready to forward AT commands", b"AT\r"),),
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
    # samples/subsys/task_wdt: the control thread gets stuck and the task
    # watchdog's callback resets the chip right after printing
    # (src/main.c:46-58), so the reset cuts the UART's transmit FIFO short: the
    # callback line ends early and "Resetting device..." never reaches the
    # console, which the yaml's last two patterns need. The callback line's
    # start, then the second boot, show the same run.
    "zephyr/subsys/task_wdt": {
        "console": (BANNER, r"Task watchdog sample application\.", r"Control thread started\.",
                    r"Main thread still alive\.\.\.", r"Control thread getting stuck\.\.\.",
                    r"Task watchdog channel 1 callback, thread: c", BANNER),
        "forbidden": FAULTS, "seconds": 20.0},
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
    # samples/drivers/gnss/src/main.c:15-27: prints each fix the driver
    # publishes; its yaml has no harness. The u-blox UBX driver maps a 2D or
    # 3D fix to GNSS_FIX_STATUS_GNSS_FIX, 1 (drivers/gnss/u_blox/gnss_ubx_common.c,
    # include/zephyr/drivers/gnss.h).
    "zephyr/drivers/gnss": own(r"Got a fix \(type: 1\)", seconds=30.0, source="samples/drivers/gnss/src/main.c"),
    # Wave 1e: what each sensor sample prints of its part's values
    # (test/zephyr_samples/parts.py), within the registers' resolution.
    # samples/sensor/mpu6050/src/main.c:52-60: die temperature (%g), then
    # acceleration (m/s/s) and rotation (rad/s), %f: 250, -500, 1125 mg and
    # 10, -2, 3 deg/s at the default full scales.
    "zephyr/sensor/mpu6050": own(
        r"^\[[0-9:.]+\]:26\.5[23]\d* Cel$", r"^  accel 2\.45\d* -4\.90\d* 11\.03\d* m/s/s$",
        r"^  gyro  0\.174\d* -0\.034\d* 0\.052\d* rad/s$",
        source="samples/sensor/mpu6050/src/main.c; harness_config none"),
    # samples/sensor/lis2dh/src/main.c:36-54: acceleration, %f m/s/s: 252,
    # -500, 1020 mg in the 4 mg steps of normal mode.
    "zephyr/sensor/lis2dh": own(
        r"x 2\.4[67]\d* , y -4\.9[01]\d* , z 10\.0[0-2]\d*",
        forbidden=(*FORBIDDEN, r"ERROR"),
        source="samples/sensor/lis2dh/src/main.c; harness_config none"),
    # samples/sensor/ina219/src/main.c:30-37: bus voltage in 4 mV steps,
    # current in the overlay's 10 uA steps.
    "zephyr/sensor/ina219": own(
        r"^Bus: 5\.12[0-4]\d* \[V\] -- Power: \S+ \[W\] -- Current: 0\.123[3-5]\d* \[A\]$",
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


# The shell's prompt on the console UART (CONFIG_SHELL_PROMPT_UART's
# default, subsys/shell/backends/Kconfig.backends).
PROMPT = r"^uart:~\$ "


def shell_spec(metadata: dict, source: str, prompt: str = PROMPT) -> Spec:
    # As twister's Shell harness (scripts/pylib/shell-twister-harness): at
    # each prompt the next command goes in with a line end, and a line of
    # its response must match its `expected` from the line's start.
    commands = metadata.get("shell_commands") or []
    return Spec(console=(*banner(metadata), prompt,
                         *(f"^(?:{c['expected']})" for c in commands if c.get("expected"))),
                input=tuple((prompt, f"{c['command']}\n".encode()) for c in commands),
                seconds=SECONDS, forbidden=FORBIDDEN, lines=True,
                source=f"{source}; samples/{metadata['sample']}/test_shell.yml")


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
    spec = Spec(console=(*banner(metadata), *patterns), seconds=SECONDS, forbidden=FORBIDDEN, lines=True,
                ordered=ordered, source=source)
    return replace(spec, **OVERRIDES.get(metadata["target"], {}))
