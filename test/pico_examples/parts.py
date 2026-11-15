# SPDX-License-Identifier: Apache-2.0 OR MIT
# The external parts a pico-examples program talks to, as harness models
# attached before the program starts. A binding names DUT pins (the
# program's defaults); the bench's fixture says which harness pin reaches
# each one. On the carrier a Pico pin can have two harness branches, and a
# PIO engine reaches 32 consecutive GPIOs from base 0 or 16, so the pins of
# one binding come from one window.

import itertools
from dataclasses import dataclass, field

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
}
