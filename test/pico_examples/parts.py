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
PARTS: dict[str, tuple[Attach, ...]] = {}


# ---- Batch B: TI INA2xx, InvenSense MPU-6050, ST LIS3DH, NXP MMA8451Q and
# MPL3115A2. Every program uses i2c_default on the Pico's default pins (SDA
# GP4, SCL GP5) at the address its source names; property values are picked
# away from the defaults and from %f rounding ties.
def _batch_b(model: str, address: int, properties: dict, **gpio) -> tuple[Attach, ...]:
    bindings = {"i2c": (4, 5, address), **{name: (pin,) for name, pin in gpio.items()}}
    return (Attach(model, model, bindings, {"i2c": 2}, properties),)


PARTS.update({
    # ina219_i2c.c:7, 21-23: 0x40, calibrated for a 0.1 ohm shunt.
    "ina219_i2c": _batch_b("ina219", 0x40, {"bus_voltage": 5124, "current": 123400, "shunt_resistance": 100000}),
    # ina228_i2c.c:23, 27: 0x40, the stock 15 mohm shunt.
    "ina228_i2c": _batch_b("ina228", 0x40, {"bus_voltage": 4850, "current": 92500, "shunt_resistance": 15000,
                                        "temperature": 41000}),
    # ina237_i2c.c:12, 26: 0x40, a 15 mohm shunt.
    "ina237_i2c": _batch_b("ina237", 0x40, {"bus_voltage": 3300, "current": 200000, "shunt_resistance": 15000,
                                        "temperature": 31250}),
    # ina260_i2c.c:11: 0x40; the shunt is inside the part.
    "ina260_i2c": _batch_b("ina260", 0x40, {"bus_voltage": 5000, "current": 742500}),
    # mpu6050_i2c.c:34: 0x68.
    "mpu6050_i2c": _batch_b("mpu6050", 0x68, {"acceleration": (250, -500, 1125),
                                          "angular_rate": (10000, -2000, 3000), "temperature": 26530}),
    # lis3dh_i2c.c:27: 0x18.
    "lis3dh_i2c": _batch_b("lis3dh", 0x18, {"acceleration": (252, -500, 1020), "temperature": 28750}),
    # mma8451_i2c.c:26: 0x1D.
    "mma8451_i2c": _batch_b("mma8451q", 0x1D, {"acceleration": (500, -250, 2000)}),
    # mpl3115a2_i2c.c:28-29: 0x60, INT1 on GP16.
    "mpl3115a2_i2c": _batch_b("mpl3115a2", 0x60, {"pressure": 95000, "altitude": 321500, "temperature": 21625},
                          int1=16),
})
# ---- end of batch B
