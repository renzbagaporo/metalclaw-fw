# SPDX-License-Identifier: Apache-2.0 OR MIT
# The ICM-42688's FIFO (DS-000347 section 6, 14.4, 14.22-14.24, 14.33,
# 14.45-14.47) over the host harness's bridge: packets at the ODR, the
# count in FIFO_COUNTH/L, burst reads of FIFO_DATA that pop them, a flush,
# a watermark, and the packets lost when it is full.

import pytest

from test.host.host import Bridge, HostHarness, default_build, spi_window

PROGRAM = default_build()
pytestmark = pytest.mark.skipif(not PROGRAM.exists(), reason=f"no host harness at {PROGRAM} (just build-host)")

SPI0 = {"bus": 0, "sck": 18, "mosi": 19, "miso": 16}
CS = 17


@pytest.fixture
def imu(tmp_path):
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    bridge.hello(spi=[SPI0])
    harness.run("import metalclaw\nimu = metalclaw.attach('icm42688', 'imu', spi=(18, 19, 16, 17))\n"
                "imu.accel = (0x1234, -2, 300)\nimu.gyro = (7, -0x1234, 0)\nimu.temperature = 640")
    yield harness, bridge
    bridge.close()
    harness.close()


def write(bridge, reg, value):
    spi_window(bridge, 0, CS, bytes([reg, value]))


def read(bridge, reg, n):
    return spi_window(bridge, 0, CS, bytes([0x80 | reg]) + bytes(n))[1:]


def count(bridge):
    return int.from_bytes(read(bridge, 0x2e, 2), "big")


def start(bridge, config1=0x07):
    write(bridge, 0x4e, 0x0f)       # PWR_MGMT0: accel and gyro low noise
    write(bridge, 0x5f, config1)    # FIFO_CONFIG1
    write(bridge, 0x16, 0x40)       # FIFO_CONFIG: stream-to-FIFO


def test_packets_of_accel_gyro_temperature_and_timestamp_stream_from_fifo_data(imu):
    harness, bridge = imu
    assert count(bridge) == 0
    assert read(bridge, 0x30, 4) == b"\xff" * 4
    start(bridge)
    bridge.advance(10_500_000)
    n = count(bridge)
    # 1 kHz, ACCEL_CONFIG0's reset ODR: a packet a millisecond.
    assert n % 16 == 0 and 9 * 16 <= n <= 12 * 16, n
    data = read(bridge, 0x30, 32)
    first, second = data[:16], data[16:]
    # Packet 3: header, accel, gyro, temperature (8 bits, 1/64 of
    # TEMP_DATA), timestamp. The first packet flags both ODRs as new.
    assert first[0] == 0x6b and second[0] == 0x68
    assert first[1:13] == bytes.fromhex("1234fffe012c" "0007edcc0000")
    assert first[13] == 10
    stamps = int.from_bytes(first[14:16], "big"), int.from_bytes(second[14:16], "big")
    assert (stamps[1] - stamps[0]) & 0xffff == 1000
    assert count(bridge) == n - 32
    assert int(harness.run("print(imu.fifo_count)")) == n - 32


def test_a_read_in_part_drops_the_packet_unless_partial_reads_resume(imu):
    _, bridge = imu
    start(bridge)
    bridge.advance(5_500_000)
    n = count(bridge)
    read(bridge, 0x30, 4)
    assert count(bridge) == n - 16
    write(bridge, 0x5f, 0x47)       # FIFO_RESUME_PARTIAL_RD
    read(bridge, 0x30, 4)
    assert count(bridge) == n - 20
    assert read(bridge, 0x30, 4)[0] not in (0x68, 0x6b)


def test_a_flush_empties_it_and_bypass_mode_takes_no_packets(imu):
    _, bridge = imu
    start(bridge)
    bridge.advance(5_500_000)
    assert count(bridge) > 0
    write(bridge, 0x4b, 0x02)       # SIGNAL_PATH_RESET: FIFO_FLUSH
    assert count(bridge) == 0
    assert read(bridge, 0x4b, 1) == b"\x00"
    write(bridge, 0x16, 0x00)
    bridge.advance(5_000_000)
    assert count(bridge) == 0


def test_the_count_in_records_and_little_endian(imu):
    _, bridge = imu
    start(bridge, config1=0x01)     # accel only: Packet 1, 8 bytes
    bridge.advance(4_500_000)
    n = count(bridge)
    assert n % 8 == 0 and n >= 32
    assert read(bridge, 0x30, 8)[:7] == bytes.fromhex("43" "1234fffe012c")
    write(bridge, 0x4c, 0x70)       # FIFO_COUNT_REC
    assert count(bridge) == (n - 8) // 8
    write(bridge, 0x4c, 0x50)       # records, little endian
    assert read(bridge, 0x2e, 2) == ((n - 8) // 8).to_bytes(2, "little")


def test_the_watermark_raises_fifo_ths_and_a_full_fifo_loses_packets(imu):
    harness, bridge = imu
    write(bridge, 0x60, 64)         # FIFO_WM, bytes
    start(bridge)
    read(bridge, 0x2d, 1)
    bridge.advance(2_500_000)
    assert read(bridge, 0x2d, 1)[0] & 0x04 == 0
    bridge.advance(3_000_000)
    assert read(bridge, 0x2d, 1)[0] & 0x04
    # 2048 bytes hold 128 packets of 16; the rest are lost and counted.
    bridge.advance(140_000_000)
    assert count(bridge) == 2048
    assert read(bridge, 0x2d, 1)[0] & 0x02
    lost = int.from_bytes(read(bridge, 0x6c, 2), "little")
    assert lost >= 5 and lost == int(harness.run("print(imu.fifo_lost)"))


def test_20_bit_packets_carry_the_low_bits_after_the_timestamp(imu):
    _, bridge = imu
    start(bridge, config1=0x17)     # FIFO_HIRES_EN: Packet 4, 20 bytes
    bridge.advance(3_500_000)
    assert count(bridge) % 20 == 0
    p = read(bridge, 0x30, 20)
    assert p[0] & 0x70 == 0x70
    # 20-bit values at the hi-res scale: accel x4, gyro x8 the 16-bit LSB.
    ax = 0x1234 * 4
    gy = (-0x1234 * 8) & 0xfffff
    assert p[1:3] == bytes([(ax >> 12) & 0xff, (ax >> 4) & 0xff])
    assert p[9:11] == bytes([(gy >> 12) & 0xff, (gy >> 4) & 0xff])
    assert p[13:15] == (640).to_bytes(2, "big")
    assert p[17] >> 4 == ax & 0xf and p[18] & 0xf == gy & 0xf


def test_a_burst_from_int_status_runs_into_fifo_data_and_pops_a_packet(imu):
    # A driver may read INT_STATUS, FIFO_COUNTH..L and a packet in one
    # burst: the address runs from 0x2F into FIFO_DATA and stays there.
    harness, bridge = imu
    start(bridge)
    bridge.advance(5_500_000)
    n = count(bridge)
    data = read(bridge, 0x2d, 3 + 16)
    assert int.from_bytes(data[1:3], "big") == n
    assert data[3] == 0x6b and data[4:16] == bytes.fromhex("1234fffe012c" "0007edcc0000")
    assert count(bridge) == n - 16
    # One from FIFO_COUNTL runs in too.
    data = read(bridge, 0x2f, 1 + 16)
    assert data[0] == (n - 16) & 0xff and data[1] == 0x68
    assert count(bridge) == n - 32
