# SPDX-License-Identifier: Apache-2.0 OR MIT

import pytest

NAME = "imu"
SCK = 18
MOSI = 19
MISO = 16
CS = 17

# What the DUT overlay configures, and the driver's own conversions from
# raw counts at those full scales: 2048 LSB/g, 16.4 LSB/dps, raw/132.48 + 25 C.
POWER_MODE_LN_BOTH = 0x0F
ACCEL_CONFIG_FS16_ODR50 = 0x09
GYRO_CONFIG_FS2000_ODR50 = 0x09
ACCEL_SENSITIVITY = 2048
GYRO_SENSITIVITY_X10 = 164
SENSOR_G = 9806650
SENSOR_PI = 3141592
TEMPERATURE_SENSITIVITY_X100 = 13248
WINDOWS = 1000

SAMPLES = [
    ((0, 0, 0), (0, 0, 0), 0),
    ((2048, -2048, 1024), (164, -164, 8200), 1325),
    ((32767, -32768, 1), (32767, -32768, -1), -32768),
    ((-1234, 5678, -9012), (3456, -7890, 12345), 32767),
]


def accel_micro(raw):
    return int(raw * SENSOR_G / ACCEL_SENSITIVITY)


def gyro_micro(raw):
    return int(raw * SENSOR_PI * 10 / (GYRO_SENSITIVITY_X10 * 180))


def temperature_micro(raw):
    return int((raw * 100 + 25 * TEMPERATURE_SENSITIVITY_X100) * 1_000_000 / TEMPERATURE_SENSITIVITY_X100)


ATTACH = f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
metalclaw.attach('icm42688', {NAME!r}, spi=({SCK}, {MOSI}, {MISO}, {CS}))
"""

SELECT = f"""import metalclaw
imu = [i for i in metalclaw.instances() if i.name == {NAME!r}][0]
"""


class Imu:
    def __init__(self, harness):
        self.harness = harness

    def set(self, accel, gyro, temperature):
        self.harness.run(SELECT + f"imu.set(accel={accel!r}, gyro={gyro!r}, temperature={temperature})\n")

    def get(self, name):
        return eval(self.harness.run(SELECT + f"print(imu.{name})\n").strip())

    def stats(self):
        return eval(self.harness.run(SELECT + "print(imu.stats('spi'))\n").strip())


@pytest.fixture(scope="module")
def imu(harness, dut):
    dut.require_image('icm4268x')
    harness.run(ATTACH)
    dut.reset()
    dut.ping()
    return Imu(harness)


def fetch(dut, count=1):
    result = dut.command(f"IMU {count}")
    assert result["test"] == "test_imu", result
    assert result["status"] == "ok", result
    return result


def assert_close(actual, expected, lsb_micro):
    assert abs(actual - expected) <= lsb_micro, (actual, expected)


@pytest.mark.parametrize("accel,gyro,temperature", SAMPLES)
def test_the_dut_driver_reads_what_was_injected(dut, imu, accel, gyro, temperature):
    imu.set(accel, gyro, temperature)
    result = fetch(dut)
    for axis in range(3):
        assert_close(result["accel_ums2"][axis], accel_micro(accel[axis]), accel_micro(1))
        assert_close(result["gyro_urads"][axis], gyro_micro(gyro[axis]), gyro_micro(1))
    assert_close(result["temp_uc"], temperature_micro(temperature), temperature_micro(1) - temperature_micro(0))


def test_the_dut_configured_the_part(dut, imu):
    fetch(dut)
    assert imu.get("power_mode") == POWER_MODE_LN_BOTH
    assert imu.get("accel_config") == ACCEL_CONFIG_FS16_ODR50
    assert imu.get("gyro_config") == GYRO_CONFIG_FS2000_ODR50
    assert imu.get("bank") == 0
    assert imu.get("resets") >= 1


def test_every_fetch_moves_bus_traffic(dut, imu):
    before = imu.stats()["transactions"]
    fetch(dut, count=10)
    assert imu.stats()["transactions"] >= before + 20


def test_a_thousand_fetches_agree(dut, imu):
    imu.set((100, 200, 300), (-100, -200, -300), 42)
    result = fetch(dut, count=WINDOWS)
    assert result["mismatches"] == 0, result
    stats = imu.stats()
    assert stats["stale_windows"] == 0 and stats["dma_faults"] == 0, stats


def test_a_dut_reset_reinitialises_the_part(dut, imu):
    resets = imu.get("resets")
    dut.reset()
    dut.ping()
    assert imu.get("resets") == resets + 1
    assert imu.get("power_mode") == POWER_MODE_LN_BOTH
    imu.set((1, 2, 3), (4, 5, 6), 7)
    result = fetch(dut)
    assert_close(result["temp_uc"], temperature_micro(7), temperature_micro(1) - temperature_micro(0))


def test_a_model_reset_clears_the_configuration(dut, imu):
    imu.harness.run(SELECT + "imu.reset()\n")
    assert imu.get("power_mode") == 0
    assert imu.get("accel") == (0, 0, 0)
    dut.reset()
    dut.ping()
    assert imu.get("power_mode") == POWER_MODE_LN_BOTH
