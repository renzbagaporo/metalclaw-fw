# SPDX-License-Identifier: Apache-2.0 OR MIT
# The external parts a pico-examples program talks to, as harness models
# attached before the program starts. A binding names DUT pins (the
# program's defaults); the bench's fixture says which harness pin reaches
# each one. On the carrier a Pico pin can have two harness branches, and a
# PIO engine reaches 32 consecutive GPIOs from base 0 or 16, so the pins of
# one binding come from one window.

import itertools
from dataclasses import dataclass, field

from test.pico_examples import displays

WINDOWS = (range(0, 32), range(16, 48))


# The harness's I2C target is an I2C controller in target mode, so SDA and
# SCL need its pin functions: I2C0 on GPIO 4k / 4k+1, I2C1 on 4k+2 / 4k+3
# (RP2040 and RP2350 GPIO function tables).
def i2c_pins(pins) -> bool:
    sda, scl = pins
    return (sda % 4, scl % 4) in ((0, 1), (2, 3))


# Per binding name, what its pins must satisfy beyond one PIO window.
RULES = {"i2c": i2c_pins}


class PinError(ValueError):
    pass


def harness_pins(fixture: dict, dut_pins, rule=None) -> tuple[int, ...]:
    reach = fixture["dut_pins"]
    missing = [p for p in dut_pins if p not in reach]
    if missing:
        raise PinError(f"no harness pin reaches DUT GP{missing}")
    for window in WINDOWS:
        options = [[b for b in reach[p] if b in window] for p in dut_pins]
        for chosen in itertools.product(*options):
            if len(set(chosen)) == len(chosen) and (rule is None or rule(chosen)):
                return tuple(chosen)
    raise PinError(f"DUT GP{list(dut_pins)} have no harness branches in one 32-pin window"
                   + (f" that suit {rule.__name__}" if rule else ""))


@dataclass(frozen=True)
class Check:
    # What a part saw of the program, read on the harness once the program
    # has run: `expression` is evaluated there with the attached instance as
    # `part` (a property, a region, a stats() field), and `test` judges the
    # value that comes back. A display's frame, an LED strip's colours, the
    # bytes a flash part was given.
    expression: str
    test: object  # callable(value) -> bool
    description: str
    # Bench seconds the program gets before the check, after a restart.
    after_seconds: float = 2.0


@dataclass(frozen=True)
class Attach:
    # metalclaw.attach(model, name, **bindings): each binding a tuple of DUT
    # pins, then any plain values (an I2C address, say) after them.
    model: str
    name: str
    bindings: dict = field(default_factory=dict)
    # How many leading entries of each binding's tuple are pins.
    pin_counts: dict = field(default_factory=dict)
    # Properties set once attached: name -> value.
    properties: dict = field(default_factory=dict)
    checks: tuple = ()

    def code(self, fixture: dict) -> str:
        args = []
        for binding, values in self.bindings.items():
            n = self.pin_counts.get(binding, len(values))
            pins = harness_pins(fixture, values[:n], RULES.get(binding))
            args.append(f"{binding}={tuple(pins) + tuple(values[n:])!r}")
        lines = [f"{self.name} = metalclaw.attach({self.model!r}, {self.name!r}, {', '.join(args)})"]
        lines += [f"{self.name}.{k} = {v!r}" for k, v in self.properties.items()]
        return "import metalclaw\n" + "\n".join(lines) + "\n"


# The parts each needs_part target uses, filled in as their models land.
# Bindings are the programs' default pins (PICO_DEFAULT_I2C_SDA_PIN 4,
# PICO_DEFAULT_I2C_SCL_PIN 5; PICO_DEFAULT_SPI_RX_PIN 16, _CSN_PIN 17,
# _SCK_PIN 18, _TX_PIN 19) and addresses; the property values are chosen to
# be unlike the models' defaults and away from %f rounding ties, so a
# reading shows it came from the rig.
def _i2c_part(model: str, address: int, properties: dict, **gpio) -> tuple[Attach, ...]:
    bindings = {"i2c": (4, 5, address), **{name: (pin,) for name, pin in gpio.items()}}
    return (Attach(model, model, bindings, {"i2c": 2}, properties),)


PARTS: dict[str, tuple[Attach, ...]] = {
    # i2c/bmp280_i2c/bmp280_i2c.c: ADDR 0x76.
    "bmp280_i2c": (
        Attach("bmp280", "baro", {"i2c": (4, 5, 0x76)}, {"i2c": 2},
               {"pressure": 98765, "temperature": 23450}),
    ),
    # i2c/mcp9808_i2c/mcp9808_i2c.c: ADDRESS 0x18. 34.562 degC is above the
    # 30 degC upper limit the program writes and under its other two.
    "mcp9808_i2c": (
        Attach("mcp9808", "thermo", {"i2c": (4, 5, 0x18)}, {"i2c": 2}, {"temperature": 34562}),
    ),
    # i2c/pcf8523_i2c/pcf8523_i2c.c: addr 0x68. The program writes its own
    # time over this one, which shows its writes land.
    "pcf8523_i2c": (
        Attach("pcf8523", "rtc", {"i2c": (4, 5, 0x68)}, {"i2c": 2},
               {"time": (26, 11, 15, 0, 12, 34, 56)}),
    ),
    # spi/bme280_spi/bme280_spi.c: spi=(SCK, MOSI = TX, MISO = RX, CSn).
    "bme280_spi": (
        Attach("bme280", "env", {"spi": (18, 19, 16, 17)}, {},
               {"pressure": 99887, "temperature": 21370, "humidity": 45670}),
    ),
    # TI INA2xx, InvenSense MPU-6050, ST LIS3DH, NXP MMA8451Q and MPL3115A2:
    # i2c_default at the address each source names.
    # ina219_i2c.c:7, 21-23: 0x40, calibrated for a 0.1 ohm shunt.
    "ina219_i2c": _i2c_part("ina219", 0x40, {"bus_voltage": 5124, "current": 123400, "shunt_resistance": 100000}),
    # ina228_i2c.c:23, 27: 0x40, the stock 15 mohm shunt.
    "ina228_i2c": _i2c_part("ina228", 0x40, {"bus_voltage": 4850, "current": 92500, "shunt_resistance": 15000,
                                         "temperature": 41000}),
    # ina237_i2c.c:12, 26: 0x40, a 15 mohm shunt.
    "ina237_i2c": _i2c_part("ina237", 0x40, {"bus_voltage": 3300, "current": 200000, "shunt_resistance": 15000,
                                         "temperature": 31250}),
    # ina260_i2c.c:11: 0x40; the shunt is inside the part.
    "ina260_i2c": _i2c_part("ina260", 0x40, {"bus_voltage": 5000, "current": 742500}),
    # mpu6050_i2c.c:34: 0x68.
    "mpu6050_i2c": _i2c_part("mpu6050", 0x68, {"acceleration": (250, -500, 1125),
                                           "angular_rate": (10000, -2000, 3000), "temperature": 26530}),
    # lis3dh_i2c.c:27: 0x18.
    "lis3dh_i2c": _i2c_part("lis3dh", 0x18, {"acceleration": (252, -500, 1020), "temperature": 28750}),
    # mma8451_i2c.c:26: 0x1D.
    "mma8451_i2c": _i2c_part("mma8451q", 0x1D, {"acceleration": (500, -250, 2000)}),
    # mpl3115a2_i2c.c:28-29: 0x60, INT1 on GP16.
    "mpl3115a2_i2c": _i2c_part("mpl3115a2", 0x60, {"pressure": 95000, "altitude": 321500, "temperature": 21625},
                           int1=16),
    # Displays: what a check reads is the controller's memory and the
    # settings the program sent, judged by test/pico_examples/displays.py.
    # i2c/ssd1306_i2c/ssd1306_i2c.c:41-44, 330-376: 0x3C, a 128 x 32 panel. The
    # raspberries are drawn about 3.1 s in (three 1 s flashes first) and
    # scroll for 5 s; the text comes after.
    "ssd1306_i2c": (
        Attach("ssd1306", "oled", {"i2c": (4, 5, 0x3C)}, {"i2c": 2}, checks=(
            Check("part.read_region('gddram', 0, 1024)", lambda v: v == displays.ssd1306_raspberries(),
                  "the three raspberries at columns 0, 31 and 62 of pages 0-3, nothing else", 5.5),
            Check("(part.display_on, part.addressing_mode, part.multiplex, part.contrast, part.segment_remap, "
                  "part.com_remap, part.com_pins, part.charge_pump, part.entire_on, part.inverse, part.scroll, "
                  "part.column_range, part.page_range)",
                  lambda v: v == (True, 0, 32, 0xFF, True, True, 0x02, True, False, False, True, (62, 87), (0, 3)),
                  "SSD1306_init()'s settings, scrolling, and the last raspberry's render area", 5.5),
        )),
    ),
    # spi/ssd1309_spi/ssd1309_spi.c:24-28: SPI0 with its own chip select,
    # D/C on GP20, RES# on GP21; spi=(SCK, MOSI, CS, D/C). The PL022 raises
    # chip select between bytes (Motorola mode, CPHA 0).
    "ssd1309_spi": (
        Attach("ssd1309", "oled", {"spi": (18, 19, 17, 20), "reset": (21,)}, checks=(
            Check("part.read_region('gddram', 0, 1024)", lambda v: displays.ssd1309_spi_frame(v, least=2),
                  "the alphabet on rows 0 and 1 and a counter of at least 2 on row 3", 2.5),
            Check("(part.display_on, part.addressing_mode)", lambda v: v == (True, 2),
                  "display on, page addressing as after RES#", 2.5),
        )),
    ),
    # spi/ssd1309_stdout_spi/ssd1309_stdout_spi.c:39-43: the same pins; a
    # DMA refresh of the whole frame every 20 ms while it changes.
    "ssd1309_stdout_spi": (
        Attach("ssd1309", "oled", {"spi": (18, 19, 17, 20), "reset": (21,)}, checks=(
            Check("part.read_region('gddram', 0, 1024)", displays.ssd1309_stdout_frame,
                  "Hello, World on row 0 from byte 16, and the snake below it", 2.0),
            Check("(part.display_on, part.addressing_mode, part.column_range, part.page_range)",
                  lambda v: v == (True, 0, (0, 127), (0, 7)),
                  "display on, horizontal addressing over the whole frame", 2.0),
        )),
    ),
    # i2c/ht16k33_i2c/ht16k33_i2c.c:37, 90-94: 0x70. The scroll runs from
    # the start for 29 steps of 150 ms.
    "ht16k33_i2c": (
        Attach("ht16k33", "leds", {"i2c": (4, 5, 0x70)}, {"i2c": 2}, checks=(
            Check("part.read_region('display', 0, 16)", displays.ht16k33_scrolling,
                  "four consecutive characters of the scrolled string on digits 0-3", 2.0),
            Check("(part.oscillator, part.display_on, part.blink, part.row_int)",
                  lambda v: v == (True, True, 0, 0), "ht16k33_init()'s settings", 2.0),
        )),
    ),
    # i2c/lcd_1602_i2c/lcd_1602_i2c.c:62: 0x27. The first pair of lines is
    # written by about 0.2 s and stays until 2.2 s.
    "lcd_1602_i2c": (
        Attach("hd44780_pcf8574", "lcd", {"i2c": (4, 5, 0x27)}, {"i2c": 2}, checks=(
            Check("part.read_region('ddram', 0, 128)", displays.lcd_showing_a_screen,
                  "a pair of the program's messages, centred, the rest of the DDRAM spaces", 1.0),
            Check("(part.display_on, part.cursor, part.blink, part.two_lines, part.font_5x10, part.eight_bit, "
                  "part.increment, part.backlight)",
                  lambda v: v == (True, False, False, True, False, False, True, True),
                  "lcd_init()'s settings in 4-bit mode, backlight on", 1.0),
        )),
    ),
    # spi/max7219_32x8_spi/max7219_32x8_spi.c:33, 92-114: SPI0, chip select
    # on GP17 by hand, four modules; spi=(SCK, MOSI, CS). The ramp of digit
    # values takes about 5.2 s.
    "max7219_32x8_spi": (
        Attach("max7219", "matrix", {"spi": (18, 19, 17)}, properties={"chain": 4}, checks=(
            Check("part.read_region('digits', 0, 32)", lambda v: displays.max7219_ramp(v, least=20),
                  "all four modules alike on one step of the 0..255 ramp, at least 20", 2.0),
            Check("(part.decode_mode[:4], part.scan_limit[:4], part.shutdown[:4], part.intensity[:4], "
                  "part.display_test[:4])",
                  lambda v: v == ((0,) * 4, (7,) * 4, (1,) * 4, (8,) * 4, (0,) * 4),
                  "the init sequence's registers on all four modules", 2.0),
        )),
    ),
    # spi/max7219_8x7seg_spi/max7219_8x7seg_spi.c:33, 110-132: one module.
    "max7219_8x7seg_spi": (
        Attach("max7219", "digits", {"spi": (18, 19, 17)}, checks=(
            Check("part.read_region('digits', 0, 8)", lambda v: displays.max7219_counting(v, least=200),
                  "a decimal count of at least 200, digit 0 the least significant", 2.0),
            Check("(part.decode_mode[0], part.scan_limit[0], part.shutdown[0], part.intensity[0], "
                  "part.display_test[0])",
                  lambda v: v == (0xFF, 7, 1, 8, 0), "the init sequence's registers", 2.0),
        )),
    ),
}
