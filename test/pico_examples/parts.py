# SPDX-License-Identifier: Apache-2.0 OR MIT
# The external parts a pico-examples program talks to, as harness models
# attached before the program starts. A binding names DUT pins (the
# program's defaults); the bench's fixture says which harness pin reaches
# each one. On the carrier a Pico pin can have two harness branches, and a
# PIO engine reaches 32 consecutive GPIOs from base 0 or 16, so the pins of
# one binding come from one window.

import itertools
from dataclasses import dataclass, field

from test.pico_examples import displays, lights

WINDOWS = (range(0, 32), range(16, 48))


# The harness's I2C target is an I2C controller in target mode, so SDA and
# SCL need its pin functions: I2C0 on GPIO 4k / 4k+1, I2C1 on 4k+2 / 4k+3
# (RP2040 and RP2350 GPIO function tables).
def i2c_pins(pins) -> bool:
    sda, scl = pins
    return (sda % 4, scl % 4) in ((0, 1), (2, 3))


# A steps_out wire drives its pins as one PIO OUT group: consecutive GPIOs.
def consecutive_pins(pins) -> bool:
    return all(p == pins[0] + i for i, p in enumerate(pins))


# Per binding name, what its pins must satisfy beyond one PIO window.
RULES = {"i2c": i2c_pins, "ab": consecutive_pins}


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
    # Run on the harness with the instance as `part` right after the
    # restart: what the part is to do for this run.
    setup: str = ""


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
    # An open-drain part with no pull-up of its own: it works only on a
    # bench whose lines have pull-ups (the carrier's 5.1 k), since a
    # harness pin's internal pull-up (50-80 k) cannot outpull the DUT pad's
    # pull-down, which gpio_init leaves on.
    needs_line_pullups: bool = False
    # The boards (example.json "board") whose build uses this part; empty
    # for every board.
    boards: tuple[str, ...] = ()
    # Every binding's pins from one PIO window together, for a part whose
    # engine links its pins (a jumper).
    one_window: bool = False
    # Properties that differ on a bench whose harness cannot hold the
    # part's pins at an analog level (fixture "analog_pins"), only high or
    # low.
    without_analog: dict = field(default_factory=dict)

    def dut_pins(self) -> tuple[int, ...]:
        return tuple(p for b, v in self.bindings.items() for p in v[:self.pin_counts.get(b, len(v))])

    def properties_for(self, fixture: dict) -> dict:
        if set(self.dut_pins()) <= set(fixture.get("analog_pins", ())):
            return self.properties
        return {**self.properties, **self.without_analog}

    def _pins(self, fixture: dict) -> dict:
        counts = {b: self.pin_counts.get(b, len(v)) for b, v in self.bindings.items()}
        if self.one_window:
            chosen = iter(harness_pins(fixture, self.dut_pins()))
            return {b: tuple(next(chosen) for _ in range(n)) for b, n in counts.items()}
        return {b: harness_pins(fixture, v[:counts[b]], RULES.get(b)) for b, v in self.bindings.items()}

    def code(self, fixture: dict) -> str:
        pins = self._pins(fixture)
        args = [f"{b}={tuple(pins[b]) + tuple(v[self.pin_counts.get(b, len(v)):])!r}"
                for b, v in self.bindings.items()]
        lines = [f"{self.name} = metalclaw.attach({self.model!r}, {self.name!r}, {', '.join(args)})"]
        lines += [f"{self.name}.{k} = {v!r}" for k, v in self.properties_for(fixture).items()]
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
    # uart/gps_uart/gps_uart.c:20-23: uart1 at 9600, the program's TX on GP4
    # and RX on GP5, so the part's TX (the binding's first pin) is GP5. The
    # program takes $GPRMC only (line 147), which a u-blox M8 sends with GPS
    # as its one GNSS (gnss=1).
    "gps_uart": (
        Attach("neo_m8n", "gps", {"uart": (5, 4)}, {},
               {"gnss": 1, "latitude": 473977420, "longitude": -1223321500, "speed": 1500, "heading": 9000000,
                "satellites": 8, "fix": 3, "time": (2026, 11, 15, 12, 34, 56)}),
    ),
    # i2c/pa1010d_i2c/pa1010d_i2c.c:26: 0x10 on i2c_default. Its PMTK 314
    # (line 125) leaves RMC alone; the part keeps what it was told.
    "pa1010d_i2c": (
        Attach("pa1010d", "gnss", {"i2c": (4, 5, 0x10)}, {"i2c": 2},
               {"latitude": 473977420, "longitude": -1223321500, "speed": 1500, "heading": 9000000,
                "satellites": 8, "fix": 3, "time": (2026, 11, 15, 12, 34, 56)},
               checks=(Check("part.nmea_rates", lambda v: tuple(v) == (0, 1, 0, 0, 0, 0),
                             "the program's PMTK 314 set RMC every fix and every other sentence off"),
                       Check("part.packets", lambda v: v >= 2, "the program read the I2C buffer"))),
    ),
    # uart/lcd_uart/lcd_uart.c:29-31: uart1 at 9600, TX only, on GP8; the
    # part's TX would reach GP9, which the program leaves alone. It prints
    # nothing. As built it sends 0xFE 0x42, Display On, and faults before
    # the argument byte: GCC turned lcd_set_backlight's read through a NULL
    # buffer (line 101) into a UDF in lcd_init (see program_parts_check.c).
    # Each run leaves a Display On waiting for its argument, which the next
    # run's 0xFE completes; no setting changes. The Pico 2's stdio_init_all
    # lets about 2.5 s pass before lcd_init.
    "lcd_uart": (
        Attach("lcd_backpack", "lcd", {"uart": (9, 8)}, {}, {},
               checks=(Check("part.stats('uart')['bytes_rx']", lambda v: v >= 2 and v % 2 == 0,
                             "each run of the program sent 0xFE 0x42 and nothing after its fault", 5.0),
                       Check("(part.contrast, part.brightness, part.columns, part.rows)",
                             lambda v: tuple(v) == (200, 255, 16, 2), "no setting changed from the factory state", 5.0),
                       Check("bytes(part.read_region('splash', 0, 16)) + bytes(part.read_region('splash', 20, 16))",
                             lambda v: v == b"\xff" * 32, "the splash is still the built-in one", 5.0))),
    ),
    # spi/mpu9250_spi/mpu9250_spi.c:43-46: MISO GP4, CS GP5, SCK GP6, MOSI GP7.
    "mpu9250_spi": (
        Attach("mpu9250", "imu", {"spi": (6, 7, 4, 5)}, {},
               {"acceleration": (-125, 750, 980), "angular_rate": (5000, -12000, 250), "temperature": 31000}),
    ),
    # spi/spi_flash/spi_flash.c and pio/spi/spi_flash.c: spi_default's pins,
    # chip select GP17. The programs end with the sector erased, so the part
    # counts what they did.
    "spi_flash": (
        Attach("w25q16jv", "flash", {"spi": (18, 19, 16, 17)}, {},
               checks=(Check("(part.programs, part.erases, part.lost, part.ignored)",
                             lambda v: v[0] >= 1 and v[1] >= 2 and v[2:] == (0, 0),
                             "the program erased, programmed and erased the first sector, nothing refused"),)),
    ),
    "pio_spi_flash": (
        Attach("w25q16jv", "flash", {"spi": (18, 19, 16, 17)}, {},
               checks=(Check("(part.programs, part.erases, part.lost, part.ignored)",
                             lambda v: v[0] >= 1 and v[1] >= 2 and v[2:] == (0, 0),
                             "the program erased, programmed and erased the first sector, nothing refused"),)),
    ),
    # Timed single-wire and clocked parts, behind the wire engines. Most of
    # these programs use GP2 and GP3, which reach no harness pin on fixture
    # 1 (they are its SWD lines), so they run on the carrier only.
    # pio/ws2812/ws2812.c:31-36, 126: WS2812_PIN 2 (the pico boards define
    # no PICO_DEFAULT_WS2812_PIN), 150 RGB pixels at 800 kHz.
    "pio_ws2812": (
        Attach("ws2812b", "strip", {"din": (2,)}, properties={"count": 150}, checks=(
            Check("part.read_region('leds', 0, 450)", lights.ws2812_shows_a_random_frame,
                  "a frame of 150 rand() pixels the 'Random data' pattern sent", 2.5),
            Check("(part.frames > 10, part.bad_frames, part.length)", lambda v: v == (True, 0, 450),
                  "frames of 450 bytes, none with a pulse out of the datasheet's windows", 2.5),
        )),
    ),
    # pio/ws2812/ws2812_parallel.c:19-20, 188-203, 295-298: strips on GP2
    # (RGB) and GP3 (RGBW), 64 pixels each, a frame of 256 values per strip.
    "pio_ws2812_parallel": (
        Attach("ws2812b", "strip0", {"din": (2,)}, properties={"count": 64, "channels": 3}, checks=(
            Check("part.read_region('leds', 0, 192)", lambda v: lights.ws2812_parallel_shows_a_frame(v, 0),
                  "strip 0 holding a frame the program dithered and sent", 2.5),
            Check("(part.frames > 10, part.bad_frames, part.length)", lambda v: v == (True, 0, 256),
                  "frames of 256 bytes, none spoiled", 2.5),
        )),
        Attach("ws2812b", "strip1", {"din": (3,)}, properties={"count": 64, "channels": 4}, checks=(
            Check("part.read_region('leds', 0, 256)", lambda v: lights.ws2812_parallel_shows_a_frame(v, 1),
                  "strip 1 holding a frame the program dithered and sent, W always 0", 2.5),
            Check("(part.frames > 10, part.bad_frames, part.length)", lambda v: v == (True, 0, 256),
                  "frames of 256 bytes, none spoiled", 2.5),
        )),
    ),
    # pio/apa102/apa102.c:14-21: CLK on GP2, DIN on GP3, 150 LEDs at 5 MHz;
    # spi=(clock, data).
    "pio_apa102": (
        Attach("apa102", "strip", {"spi": (2, 3)}, properties={"count": 150}, checks=(
            Check("part.read_region('leds', 0, 600)", lights.apa102_shows_a_frame,
                  "one step of the three sine waves, at global brightness 16", 2.0),
            Check("(part.frames > 10, part.end_frames > 10)", lambda v: v == (True, True),
                  "start and end frames around every update", 2.0),
        )),
    ),
    # pio/onewire/onewire.c:21-22: the bus on GP15. 21.375 degC is 342
    # sixteenths, exact at the default 12 bits.
    "pio_onewire": (
        Attach("ds18b20", "thermo", {"dq": (15,)}, properties={"temperature": 21375, "serial": 0xC0FFEE},
               checks=(
                   Check("(part.searches > 0, part.matches > 1, part.conversions > 1, "
                         "part.scratchpad_reads > 1)", lambda v: v == (True, True, True, True),
                         "a search that found the part, conversions, and its scratchpad read by its ROM", 4.0),
               ), needs_line_pullups=True),
    ),
    # gpio/dht_sensor/dht.c:16: the sensor on GP15.
    "dht": (
        Attach("dht22", "sensor", {"data": (15,)}, properties={"humidity": 45600, "temperature": 23400},
               checks=(Check("part.responses > 1", lambda v: v is True,
                             "the program's start signals answered", 5.0),),
               needs_line_pullups=True),
    ),
    # pio/quadrature_encoder/quadrature_encoder.c:41 and the substep
    # program's :371: A on GP10, B on GP11. A thousand steps a second, back
    # and forth over 2000.
    "pio_quadrature_encoder": (
        Attach("quadrature_encoder", "encoder", {"ab": (10, 11)}, properties={"travel": 2000, "rate": 1000},
               checks=(Check("part.steps > 2000", lambda v: v is True, "the harness turned it", 3.0),)),
    ),
    "pio_quadrature_encoder_substep": (
        Attach("quadrature_encoder", "encoder", {"ab": (10, 11)}, properties={"travel": 2000, "rate": 1000},
               checks=(Check("part.steps > 2000", lambda v: v is True, "the harness turned it", 3.0),)),
    ),
    # pio/ir_nec/ir_loopback/ir_loopback.c:24-25: the IR LED on GP14, the
    # receiver's output on GP15; ir=(led, out).
    "pio_ir_loopback": (
        Attach("tsop38238", "receiver", {"ir": (14, 15)}, checks=(
            Check("(part.frames > 1, part.address)", lambda v: v == (True, 0),
                  "NEC frames to address 0 through the IR path", 3.5),
        )),
    ),
    # pio/hub75/hub75.c:16-22: R0 G0 B0 R1 G1 B1 on GP0-5, A-E on GP6-10,
    # CLK GP11, STROBE (latch) GP12, OEn GP13; clock=(clk, latch, oe).
    "pio_hub75": (
        Attach("hub75", "panel", {"clock": (11, 12, 13), "upper": (0, 1, 2), "lower": (3, 4, 5),
                                  "rows": (6, 7, 8, 9), "e": (10,)}, checks=(
            Check("part.slots", lambda v: v == 256, "all 32 rows seen with their 8 enable times", 8.0),
            Check("part.read_region('thumbnail', 0, 3072)", lights.hub75_shows_the_mountains,
                  "every eighth column of the mountains, gamma-corrected, within 2 counts", 8.0),
        )),
    ),
}


# What a program finds on its pins beyond a part: a signal on an input, or a
# jumper between two of its own pins. Times are in ns, the square_wave
# model's unit; a wave whose period is under 2 ms comes from the harness's
# PWM, a longer one follows the simulation clock.
MS = 1_000_000


def _wave(name: str, pin: int, period: int, high: int, **variants) -> Attach:
    return Attach("square_wave", name, {"out": (pin,)}, properties={"period": period, "high": high}, **variants)


def _level(level: bool) -> dict:
    return {"period": 0, "high": 0, "level": level}


def _jumper(name: str, source: int, sink: int) -> Attach:
    return Attach("jumper", name, {"source": (source,), "sink": (sink,)}, one_window=True)


def _i2c_jumpers() -> tuple[Attach, ...]:
    # slave_mem_i2c.c:20-23: the i2c0 target on GP4 (SDA) / GP5 (SCL), the
    # i2c1 controller on GP6 / GP7, wired SDA to SDA and SCL to SCL.
    return (Attach("jumper_open_drain", "sda", {"a": (4,), "b": (6,)}, one_window=True),
            Attach("jumper_open_drain", "scl", {"a": (5,), "b": (7,)}, one_window=True))


# low_power_{dormant,sleep,pstate}_gpio.c wait while LOW_POWER_WAKE_GPIO 15
# is high, go to sleep once it is low, and wake on its rising edge. GP15
# rests high and dips low for 100 ms a second: every dip is one sleep and
# one wake, and the DUT is reachable over SWD but for the dips.
WAKE = _wave("wake", 15, 1000 * MS, 900 * MS)

STIMULI: dict[str, tuple[Attach, ...]] = {
    "low_power_dormant_gpio": (WAKE,),
    "low_power_sleep_gpio": (WAKE,),
    "low_power_pstate_gpio": (WAKE,),
    # gpio/hello_gpio_irq/hello_gpio_irq.c:11, 29: GPIO_WATCH_PIN 2 with
    # an interrupt on both edges; an edge every 200 ms.
    "hello_gpio_irq": (_wave("edges", 2, 400 * MS, 200 * MS),),
    # gpio/hello_7segment/hello_7segment.c:28-29, 71-79: the button on
    # FIRST_GPIO + 7 = GP9, pressed when low; 2 s pressed, 2 s released.
    "hello_7segment": (_wave("button", 9, 4000 * MS, 2000 * MS),),
    # low_power_dormant_timer.c:25-29, 66-68 on RP2040: the AON timer runs
    # from RTC_CLOCK_SRC_GPIO_IN 20 at RTC_CLOCK_FREQ_HZ, 46875 Hz
    # (pico-sdk pico_low_power), clk_usb / 1024 on a Pico.
    "low_power_dormant_timer": (_wave("rtc_clock", 20, 21333, 10667, boards=("pico",)),),
    # adc/joystick_display/joystick_display.c:15-21: X on GP26 (ADC0), Y on
    # GP27 (ADC1). A 1 MHz PWM through the carrier's RC filter holds X at a
    # quarter and Y at three quarters of the harness's 3V3; a harness pin
    # straight on the DUT's can only hold X low and Y high.
    "joystick_display": (_wave("x", 26, 1000, 250, without_analog=_level(False)),
                         _wave("y", 27, 1000, 750, without_analog=_level(True))),
    # adc/microphone_adc/microphone_adc.c:24-25: ADC0 on GP26, at 40 %, or
    # low.
    "microphone_adc": (_wave("mic", 26, 1000, 400, without_analog=_level(False)),),
}

JUMPERS: dict[str, tuple[Attach, ...]] = {
    # pio/uart_rx/uart_rx.c:26-27, uart_rx_intr.c:33-34: uart1 TX on GP4 to
    # the PIO receiver on GP3.
    "pio_uart_rx": (_jumper("tx_to_rx", 4, 3),),
    "pio_uart_rx_intr": (_jumper("tx_to_rx", 4, 3),),
    # pio/uart_dma/uart_dma.c:21-22: the PIO transmitter on GP4 to the PIO
    # receiver on GP5.
    "pio_uart_dma": (_jumper("tx_to_rx", 4, 5),),
    # pio/manchester_encoding/manchester_encoding.c:17-18 and
    # pio/differential_manchester/differential_manchester.c: GP2 to GP3.
    "pio_manchester_encoding": (_jumper("tx_to_rx", 2, 3),),
    "pio_differential_manchester": (_jumper("tx_to_rx", 2, 3),),
    # pwm/measure_duty_cycle/measure_duty_cycle.c:16-17: PWM out on GP2,
    # measured on GP5 (PWM 2B).
    "pwm_measure_duty_cycle": (_jumper("out_to_in", 2, 5),),
    # pio/clocked_input/clocked_input.c:23-30: SPI0 SCK GP2 to the PIO's
    # clock GP5, SPI0 TX GP3 to its data GP4.
    "pio_clocked_input": (_jumper("clock", 2, 5), _jumper("data", 3, 4)),
    "slave_mem_i2c": _i2c_jumpers(),
    "slave_mem_i2c_burst": _i2c_jumpers(),
}

PARTS.update(STIMULI)
PARTS.update(JUMPERS)


# spi/spi_master_slave: spi_master.c and spi_slave.c are each other's part,
# SPI0 at 1 MHz in mode 0 on the default pins, one byte per chip-select
# window (spi_peer.h). The master's MOSI is its TX, GP19, and its MISO its
# RX, GP16; the slave receives on its RX, GP16, and answers on its TX, GP19.
UP = bytes(range(256))
DOWN = bytes(255 - i for i in range(256))

PEERS: dict[str, tuple[Attach, ...]] = {
    # spi_master.c:79-89: one page every 10 s, which the target answers with
    # the slave's ~i (spi_slave.c:72-75).
    "spi_master": (Attach("spi_page_target", "peer", {"sck": (18,), "mosi": (19,), "miso": (16,), "cs": (17,)},
                          one_window=True,
                          checks=(Check("part.read_region('mosi', 0, 256)", lambda v: v == UP,
                                        "the program clocked out its 00..ff page"),)),),
    # spi_slave.c:72-88: a page of ~i out for every page in; the controller
    # clocks the master's 00..ff (spi_master.c:70-73) when told to.
    "spi_slave": (Attach("spi_page_controller", "peer", {"sck": (18,), "mosi": (16,), "miso": (19,), "cs": (17,)},
                         one_window=True,
                         checks=(Check("part.read_region('miso', 0, 256)", lambda v: v == DOWN,
                                       "the program answered a page with its ff..00", setup="part.pages = 1"),)),),
}

PARTS.update(PEERS)


# The parts a program's build on `board` (example.json "board") uses, but
# those named in `replaced`, which the bench provides by other means.
def parts_for(target: str, board: str | None, replaced=()) -> tuple[Attach, ...]:
    return tuple(p for p in PARTS.get(target, ())
                 if (not p.boards or board in p.boards) and p.name not in replaced)


# The DUT pins they take.
def parts_dut_pins(target: str, board: str | None) -> set[int]:
    return {pin for part in parts_for(target, board) for pin in part.dut_pins()}
