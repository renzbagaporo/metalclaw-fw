# SPDX-License-Identifier: Apache-2.0 OR MIT

import pytest

NAME = "baro"
SDA = 4
SCL = 5
ADDRESS = 0x76

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

ATTACH = f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
metalclaw.attach('bmp388', {NAME!r}, i2c=({SDA}, {SCL}, {ADDRESS:#04x}))
"""

SELECT = f"""import metalclaw
baro = [i for i in metalclaw.instances() if i.name == {NAME!r}][0]
"""


class Barometer:
    def __init__(self, harness):
        self.harness = harness

    def set(self, pressure, temperature):
        self.harness.run(SELECT + f"baro.set(pressure={pressure}, temperature={temperature})\n")

    def get(self, name):
        return int(self.harness.run(SELECT + f"print(baro.{name})\n").strip())


@pytest.fixture(scope="module")
def barometer(harness, dut):
    harness.run(ATTACH)
    dut.reset()
    dut.ping()
    return Barometer(harness)


def read_sample(dut):
    result = dut.command("SENSOR")
    assert result["test"] == "test_sensor", result
    assert result["status"] == "ok", result
    return result["pressure_pa"], result["temperature_mc"]


@pytest.mark.parametrize("pressure,temperature", SAMPLES)
def test_the_dut_driver_reads_what_was_injected(dut, barometer, pressure, temperature):
    barometer.set(pressure, temperature)
    read_pressure, read_temperature = read_sample(dut)
    assert abs(read_pressure - pressure) <= PRESSURE_TOLERANCE_PA, (read_pressure, pressure)
    assert abs(read_temperature - temperature) <= TEMPERATURE_TOLERANCE_MC, (read_temperature, temperature)


def test_a_new_sample_reaches_the_dut(dut, barometer):
    barometer.set(101325, 25000)
    first = read_sample(dut)
    barometer.set(87000, 41250)
    second = read_sample(dut)
    assert first != second
    assert abs(second[0] - 87000) <= PRESSURE_TOLERANCE_PA, second
    assert abs(second[1] - 41250) <= TEMPERATURE_TOLERANCE_MC, second


def test_the_model_reports_what_it_was_set_to(barometer):
    barometer.set(64000, -7000)
    assert barometer.get("pressure") == 64000
    assert barometer.get("temperature") == -7000


def test_every_fetch_moves_bus_traffic(dut, barometer):
    before = barometer.get("transactions")
    read_sample(dut)
    assert barometer.get("transactions") > before


def test_the_dut_configured_the_part(dut, barometer):
    assert barometer.get("power_mode") == POWER_MODE_NORMAL
    assert barometer.get("osr") == OSR_PRESS_X4_TEMP_X1
    assert barometer.get("odr") == ODR_50_HZ


def test_a_reset_clears_what_the_dut_configured(harness, dut, barometer):
    barometer.set(70000, 12000)
    harness.run(SELECT + "baro.reset()\n")
    assert barometer.get("pressure") == 101325
    assert barometer.get("temperature") == 25000
    assert barometer.get("power_mode") == 0
    dut.reset()
    dut.ping()
    assert barometer.get("power_mode") == POWER_MODE_NORMAL
    assert barometer.get("odr") == ODR_50_HZ
    assert abs(read_sample(dut)[0] - 101325) <= PRESSURE_TOLERANCE_PA
