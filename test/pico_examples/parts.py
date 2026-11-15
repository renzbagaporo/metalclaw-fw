# SPDX-License-Identifier: Apache-2.0 OR MIT
# The external parts a pico-examples program talks to, as harness models
# attached before the program starts. A binding names DUT pins (the
# program's defaults); the bench's fixture says which harness pin reaches
# each one. On the carrier a Pico pin can have two harness branches, and a
# PIO engine reaches 32 consecutive GPIOs from base 0 or 16, so the pins of
# one binding come from one window.

import itertools
from dataclasses import dataclass, field

from test.pico_examples import lights

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
    # An open-drain part with no pull-up of its own: it works only on a
    # bench whose lines have pull-ups (the carrier's 5.1 k), since a
    # harness pin's internal pull-up (50-80 k) cannot outpull the DUT pad's
    # pull-down, which gpio_init leaves on.
    needs_line_pullups: bool = False

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

