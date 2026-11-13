# SPDX-License-Identifier: Apache-2.0 OR MIT

import struct

import pytest

NAME = "baro"
FAMILY = "bmp388"
ALIAS = "barometer"
IMAGES = ("bmp388", "bmp388/spi")

PRESSURE_TOLERANCE_PA = 1
TEMPERATURE_TOLERANCE_MC = 1

POWER_MODE_NORMAL = 3
OSR_PRESS_X4_TEMP_X1 = 0x02
ODR_50_HZ = 2

SAMPLES = [
    (101325, 25000),
    (90000, -12500),
    (30000, 85000),
    (125000, -40000),
]


# The same model on either bus; which one is a binding line, nothing else.
def attach(fixture, image):
    if image.endswith("/spi"):
        spi = fixture["spi"]
        pins = (spi["sck"], spi["mosi"], spi["miso"], spi["cs"][FAMILY])
        return f"metalclaw.attach('bmp388', {NAME!r}, spi={pins})"
    i2c = fixture["i2c"]
    bus = (i2c["sda"], i2c["scl"], i2c["address"][FAMILY])
    return f"metalclaw.attach('bmp388', {NAME!r}, i2c={bus})"


DETACH = f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
"""

SELECT = f"""import metalclaw
baro = [i for i in metalclaw.instances() if i.name == {NAME!r}][0]
"""


class Barometer:
    def __init__(self, harness, bus):
        self.harness = harness
        self.bus = bus

    def set(self, pressure, temperature):
        self.harness.run(SELECT + f"baro.set(pressure={pressure}, temperature={temperature})\n")

    def get(self, name):
        return int(self.harness.run(SELECT + f"print(baro.{name})\n").strip())

    def stats(self):
        return eval(self.harness.run(SELECT + f"print(baro.stats({self.bus!r}))\n").strip())

    def transactions(self):
        return self.stats()["transactions"]


# On I2C the DUT runs Zephyr's bmp388 driver and the SENSOR command reports
# what it computed. Zephyr's SPI backend for the part cannot initialise (its
# reads are capped at six bytes, the calibration block is 21), so on SPI the
# DUT clocks the driver's own sequence by hand through the RAW command and
# the compensation below is the driver's, ported. Each reader keeps what the
# DUT returned for its last read in `last`.
class DriverReader:
    def __init__(self, dut):
        self.dut = dut
        self.last = None

    def boot(self):
        pass

    def read(self):
        result = self.dut.command(f"SENSOR {ALIAS} press ambient_temp")
        assert result["test"] == "test_sensor", result
        assert result["status"] == "ok", result
        self.last = result
        return result["press"][0] // 1000, result["ambient_temp"][0] // 1000


def cdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


class RawReader:
    REG_CHIP_ID = 0x00
    REG_ERR = 0x02
    REG_DATA = 0x04
    REG_PWR_CTRL = 0x1B
    REG_OSR = 0x1C
    REG_ODR = 0x1D
    REG_CONFIG = 0x1F
    REG_CALIB = 0x31
    REG_CMD = 0x7E
    CMD_SOFT_RESET = 0xB6
    PWR_CTRL_ON = 0x33

    def __init__(self, dut):
        self.dut = dut
        self.cal = None
        self.last = None

    def raw(self, tx, write_only=False):
        result = self.dut.command("RAW " + tx.hex() + (" w" if write_only else ""))
        assert result["test"] == "test_raw", result
        assert result["status"] == "ok", result
        return bytes.fromhex(result["rx"])

    def read_regs(self, reg, count):
        return self.raw(bytes([reg | 0x80]) + bytes(count + 1))[2:]

    def write_reg(self, reg, value):
        self.raw(bytes([reg & 0x7F, value]), write_only=True)

    def boot(self):
        self.write_reg(self.REG_CMD, self.CMD_SOFT_RESET)
        chip_id = self.read_regs(self.REG_CHIP_ID, 1)[0]
        assert chip_id == 0x50, hex(chip_id)
        block = self.read_regs(self.REG_CALIB, 21)
        self.cal = struct.unpack("<HHbhhbbHHbbhbb", block)
        odr = self.read_regs(self.REG_ODR, 1)[0]
        self.write_reg(self.REG_ODR, (odr & ~0x1F) | ODR_50_HZ)
        self.write_reg(self.REG_OSR, OSR_PRESS_X4_TEMP_X1)
        self.write_reg(self.REG_CONFIG, 0)
        self.write_reg(self.REG_PWR_CTRL, self.PWR_CTRL_ON)
        err = self.read_regs(self.REG_ERR, 1)[0]
        assert err & 0x04 == 0, hex(err)

    def read(self):
        data = self.read_regs(self.REG_DATA, 6)
        self.last = data.hex(" ")
        raw_press = data[0] | (data[1] << 8) | (data[2] << 16)
        raw_temp = data[3] | (data[4] << 8) | (data[5] << 16)
        t1, t2, t3, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11 = self.cal
        partial1 = raw_temp - 256 * t1
        partial2 = t2 * partial1
        partial3 = partial1 * partial1
        partial4 = partial3 * t3
        comp_temp = cdiv(partial2 * 262144 + partial4, 4294967296)
        temperature_uc = cdiv(comp_temp * 250000, 16384)
        t_lin = comp_temp
        partial1 = t_lin * t_lin
        partial2 = cdiv(partial1, 64)
        partial3 = cdiv(partial2 * t_lin, 256)
        partial4 = cdiv(p8 * partial3, 32)
        partial5 = (p7 * partial1) * 16
        partial6 = (p6 * t_lin) * 4194304
        offset = p5 * 140737488355328 + partial4 + partial5 + partial6
        partial2 = cdiv(p4 * partial3, 32)
        partial4 = (p3 * partial1) * 4
        partial5 = (p2 - 16384) * t_lin * 2097152
        sensitivity = (p1 - 16384) * 70368744177664 + partial2 + partial4 + partial5
        partial1 = cdiv(sensitivity, 16777216) * raw_press
        partial2 = p10 * t_lin
        partial3 = partial2 + 65536 * p9
        partial4 = cdiv(partial3 * raw_press, 8192)
        partial5 = cdiv(raw_press * cdiv(partial4, 10), 512) * 10
        partial6 = raw_press * raw_press
        partial2 = cdiv(p11 * partial6, 65536)
        partial3 = cdiv(partial2 * raw_press, 128)
        partial4 = cdiv(offset, 4) + partial1 + partial5 + partial3
        comp_press = (partial4 * 25) // 1099511627776
        pressure_pa = comp_press // 100000 * 1000 + (comp_press % 100000) * 10 // 1000
        return pressure_pa, cdiv(temperature_uc, 1000)


READERS = {"bmp388": DriverReader, "bmp388/spi": RawReader}


@pytest.fixture(scope="module")
def barometer(harness, dut, fixture):
    image = dut.image_name()
    if image not in IMAGES:
        pytest.skip(f"the DUT image is not built for bmp388 ({image})")
    harness.run(DETACH + attach(fixture, image) + "\n")
    dut.reset()
    dut.ping()
    return Barometer(harness, "spi" if image.endswith("/spi") else "i2c")


@pytest.fixture(scope="module")
def reader(dut, barometer):
    r = READERS[dut.image_name()](dut)
    r.boot()
    return r


@pytest.mark.parametrize("pressure,temperature", SAMPLES)
def test_the_dut_driver_reads_what_was_injected(reader, barometer, pressure, temperature):
    barometer.set(pressure, temperature)
    read_pressure, read_temperature = reader.read()
    # Read before any assert, so a wrong value shows the bytes the DUT got
    # and the windows the harness saw; plain strings, since pytest elides
    # nested containers.
    seen = f"read {reader.last!r} stats {barometer.stats()!r}"
    assert abs(read_pressure - pressure) <= PRESSURE_TOLERANCE_PA, \
        f"{(read_pressure, pressure)!r} {seen}"
    assert abs(read_temperature - temperature) <= TEMPERATURE_TOLERANCE_MC, \
        f"{(read_temperature, temperature)!r} {seen}"


def test_a_new_sample_reaches_the_dut(reader, barometer):
    barometer.set(101325, 25000)
    first = reader.read()
    barometer.set(87000, 41250)
    second = reader.read()
    assert first != second
    assert abs(second[0] - 87000) <= PRESSURE_TOLERANCE_PA, second
    assert abs(second[1] - 41250) <= TEMPERATURE_TOLERANCE_MC, second


def test_the_model_reports_what_it_was_set_to(barometer):
    barometer.set(64000, -7000)
    assert barometer.get("pressure") == 64000
    assert barometer.get("temperature") == -7000


def test_every_fetch_moves_bus_traffic(reader, barometer):
    before = barometer.transactions()
    reader.read()
    assert barometer.transactions() > before


def test_the_dut_configured_the_part(reader, barometer):
    assert barometer.get("power_mode") == POWER_MODE_NORMAL
    assert barometer.get("osr") == OSR_PRESS_X4_TEMP_X1
    assert barometer.get("odr") == ODR_50_HZ


def test_a_reset_clears_what_the_dut_configured(harness, dut, barometer, reader):
    barometer.set(70000, 12000)
    harness.run(SELECT + "baro.reset()\n")
    assert barometer.get("pressure") == 101325
    assert barometer.get("temperature") == 25000
    assert barometer.get("power_mode") == 0
    dut.reset()
    dut.ping()
    reader.boot()
    assert barometer.get("power_mode") == POWER_MODE_NORMAL
    assert barometer.get("odr") == ODR_50_HZ
    assert abs(reader.read()[0] - 101325) <= PRESSURE_TOLERANCE_PA
