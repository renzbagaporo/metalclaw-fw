# SPDX-License-Identifier: Apache-2.0 OR MIT

import pytest

NAME = "imu"
FAMILY = "icm4268x"
FIFO_IMAGE = "icm4268x/fifo"
ALIAS = "imu"
CHANNELS = "accel_xyz gyro_xyz die_temp"

# What the DUT overlay configures, and the driver's own conversions from
# raw counts at those full scales: 2048 LSB/g, 16.4 LSB/dps, raw/132.48 + 25 C.
POWER_MODE_LN_BOTH = 0x0F
ACCEL_CONFIG_FS16_ODR500 = 0x0F
GYRO_CONFIG_FS2000_ODR500 = 0x0F
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


def attach(fixture):
    spi = fixture["spi"]
    pins = (spi["sck"], spi["mosi"], spi["miso"], spi["cs"][FAMILY])
    return f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
metalclaw.attach('icm42688', {NAME!r}, spi={pins})
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
def imu(harness, dut, fixture):
    dut.require_image('icm4268x')
    harness.run(attach(fixture))
    dut.reset()
    dut.ping()
    return Imu(harness)


def fetch(dut, count=1):
    result = dut.command(f"SENSOR {ALIAS} {CHANNELS} count={count}")
    assert result["test"] == "test_sensor", result
    assert result["status"] == "ok", result
    return result


def assert_close(actual, expected, lsb_micro):
    assert abs(actual - expected) <= lsb_micro, (actual, expected)


@pytest.mark.parametrize("accel,gyro,temperature", SAMPLES)
def test_the_dut_driver_reads_what_was_injected(dut, imu, accel, gyro, temperature):
    imu.set(accel, gyro, temperature)
    result = fetch(dut)
    for axis in range(3):
        assert_close(result["accel_xyz"][axis], accel_micro(accel[axis]), accel_micro(1))
        assert_close(result["gyro_xyz"][axis], gyro_micro(gyro[axis]), gyro_micro(1))
    assert_close(result["die_temp"][0], temperature_micro(temperature), temperature_micro(1) - temperature_micro(0))


def test_the_dut_configured_the_part(dut, imu):
    fetch(dut)
    assert imu.get("power_mode") == POWER_MODE_LN_BOTH
    assert imu.get("accel_config") == ACCEL_CONFIG_FS16_ODR500
    assert imu.get("gyro_config") == GYRO_CONFIG_FS2000_ODR500
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
    assert_close(result["die_temp"][0], temperature_micro(7), temperature_micro(1) - temperature_micro(0))


def test_a_model_reset_clears_the_configuration(dut, imu):
    imu.harness.run(SELECT + "imu.reset()\n")
    assert imu.get("power_mode") == 0
    assert imu.get("accel") == (0, 0, 0)
    dut.reset()
    dut.ping()
    assert imu.get("power_mode") == POWER_MODE_LN_BOTH


# The FIFO through the bare-SPI image (DS-000347 section 6, 14.22-14.24,
# 14.45): the cases write the registers and read FIFO_COUNT and FIFO_DATA
# themselves. ACCEL_CONFIG0 0x0B is 12.5 Hz, a packet every 80 ms, and the
# sensors are switched off before reading, so the FIFO holds still.
REG_FIFO_CONFIG = 0x16
REG_FIFO_COUNTH = 0x2E
REG_FIFO_DATA = 0x30
REG_SIGNAL_PATH_RESET = 0x4B
REG_PWR_MGMT0 = 0x4E
REG_ACCEL_CONFIG0 = 0x50
REG_FIFO_CONFIG1 = 0x5F
PACKET3 = 16
# RAW clocks the command byte and the data on in one go, with none of the
# pause the driver's two chunks leave between them for the engine's
# register lookup: 1 MHz leaves it time.
RAW_HZ = 1_000_000
FIFO_ACCEL = (0x1234, -2, 300)
FIFO_GYRO = (7, -0x1234, 0)
FIFO_TEMPERATURE = 640


def raw_window(dut, frame, write=False, count=1, gap_us=0):
    line = f"RAW {frame}" + (" w" if write else "") + f" {RAW_HZ}" + (f" n={count} gap={gap_us}" if count > 1 else "")
    result = dut.command(line, 60.0)
    assert result["test"] == "test_raw" and result["status"] == "ok", result
    return result


def reg_write(dut, reg, value):
    raw_window(dut, f"{reg:02x}{value:02x}", write=True)


def reg_read(dut, reg, n):
    return bytes.fromhex(raw_window(dut, f"{0x80 | reg:02x}" + "00" * n)["rx"])[1:]


def fifo_count(dut):
    return int.from_bytes(reg_read(dut, REG_FIFO_COUNTH, 2), "big")


@pytest.fixture(scope="module")
def fifo(harness, dut, fixture):
    dut.require_image(FIFO_IMAGE)
    harness.run(attach(fixture))
    imu = Imu(harness)
    imu.set(FIFO_ACCEL, FIFO_GYRO, FIFO_TEMPERATURE)
    return imu


def fill(dut, seconds):
    import time
    reg_write(dut, REG_SIGNAL_PATH_RESET, 0x02)
    reg_write(dut, REG_ACCEL_CONFIG0, 0x0B)
    reg_write(dut, REG_FIFO_CONFIG1, 0x07)
    reg_write(dut, REG_FIFO_CONFIG, 0x40)
    reg_write(dut, REG_PWR_MGMT0, 0x0F)
    time.sleep(seconds)
    reg_write(dut, REG_PWR_MGMT0, 0x00)
    return fifo_count(dut)


def test_fifo_packets_read_back_byte_for_byte_and_pop(dut, fifo):
    n = fill(dut, 1.0)
    assert n % PACKET3 == 0 and 8 * PACKET3 <= n <= 20 * PACKET3, n
    packets = []
    while fifo_count(dut) > 0 and len(packets) < 24:
        packets.append(reg_read(dut, REG_FIFO_DATA, PACKET3))
    assert len(packets) == n // PACKET3
    payload = bytes.fromhex("1234fffe012c" "0007edcc0000") + bytes([FIFO_TEMPERATURE // 64])
    for i, p in enumerate(packets):
        assert p[0] == (0x6B if i == 0 else 0x68), (i, p.hex())
        assert p[1:14] == payload, (i, p.hex())
    steps = {(int.from_bytes(b[14:], "big") - int.from_bytes(a[14:], "big")) & 0xFFFF
             for a, b in zip(packets, packets[1:])}
    assert steps == {80000 & 0xFFFF}, steps
    # Empty, FIFO_DATA reads 0xFF.
    assert reg_read(dut, REG_FIFO_DATA, 4) == b"\xff" * 4
    stats = fifo.stats()
    assert stats["dma_faults"] == 0 and stats["short_windows"] == 0, stats


def test_back_to_back_fifo_reads_each_take_the_next_packet(dut, fifo):
    # Eight reads in a row: each pop lands in the interrupt at the window's
    # end, before the next window starts.
    n = fill(dut, 1.0)
    assert n >= 8 * PACKET3, n
    result = raw_window(dut, f"{0x80 | REG_FIFO_DATA:02x}" + "00" * PACKET3, count=8, gap_us=50)
    assert fifo_count(dut) == n - 8 * PACKET3, result
    # The packets differ only in their timestamps, so every later read
    # differs from the first.
    assert result["mismatches"] == 7, result


def test_a_burst_from_int_status_runs_into_fifo_data(dut, fifo):
    # INT_STATUS, FIFO_COUNTH..L and a packet in one burst, the address
    # held at FIFO_DATA once it gets there, as icm426xx reads them.
    n = fill(dut, 1.0)
    assert n >= 2 * PACKET3, n
    data = reg_read(dut, 0x2D, 3 + PACKET3)
    assert int.from_bytes(data[1:3], "big") == n, data.hex()
    assert data[3] == 0x68 and data[4:16] == bytes.fromhex("1234fffe012c" "0007edcc0000"), data.hex()
    assert fifo_count(dut) == n - PACKET3
