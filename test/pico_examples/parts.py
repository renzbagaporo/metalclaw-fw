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
