# SPDX-License-Identifier: Apache-2.0 OR MIT

import time

import pytest

NAME = "gnss"
TX = 0
RX = 1

CURRENT_SPEED = 115200
INITIAL_BAUD = 9600
FIX_RATE_MS = 1000

FIX_STATUS_NO_FIX = 0
FIX_STATUS_GNSS_FIX = 1
FIX_QUALITY_INVALID = 0
FIX_QUALITY_GNSS_SPS = 1
NAVIGATION_MODE_BALANCED = 2
SYSTEM_GPS = 1
SYSTEM_GLONASS = 2
SYSTEM_GALILEO = 4
SYSTEM_BEIDOU = 8

# latitude, longitude (1e-7 deg), altitude (mm), speed (mm/s), heading (1e-5 deg), satellites, time
FIXES = [
    (473977420, -1223321500, 52000, 1500, 9000000, 9, (2026, 9, 13, 5, 42, 7)),
    (-338688200, 1512092900, -3000, 0, 0, 4, (2026, 1, 31, 23, 59, 59)),
    (900000000, 1800000000, 8848860, 250000, 35999999, 32, (2015, 1, 1, 0, 0, 0)),
    (0, 0, 0, 0, 27000000, 12, (2000, 2, 29, 12, 30, 0)),
]

ATTACH = f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
metalclaw.attach('neo_m8n', {NAME!r}, uart=({TX}, {RX}))
"""

SELECT = f"""import metalclaw
gnss = [i for i in metalclaw.instances() if i.name == {NAME!r}][0]
"""


class Receiver:
    def __init__(self, harness):
        self.harness = harness

    def set(self, **values):
        args = ", ".join(f"{k}={v!r}" for k, v in values.items())
        self.harness.run(SELECT + f"gnss.set({args})\n")

    def get(self, name):
        text = self.harness.run(SELECT + f"print(gnss.{name})\n").strip()
        return eval(text)

    def stats(self):
        return eval(self.harness.run(SELECT + "print(gnss.stats('uart'))\n").strip())

    def advance(self, ms):
        self.harness.run(f"import metalclaw\nmetalclaw.advance_us({ms * 1000})\n")
        time.sleep(0.2)

    def reset(self):
        self.harness.run(SELECT + "gnss.reset()\n")


@pytest.fixture(scope="module")
def receiver(harness, dut):
    dut.require_image('neo_m8n')
    harness.run(ATTACH)
    dut.reset()
    dut.ping()
    return Receiver(harness)


def gnss(dut, sub=None):
    result = dut.command("GNSS" if sub is None else f"GNSS {sub}")
    assert result["test"] == "test_gnss", result
    return result


def test_the_dut_driver_negotiated_and_configured_the_part(dut, receiver):
    assert gnss(dut)["status"] == "ok"
    assert receiver.get("baud") == CURRENT_SPEED
    assert receiver.get("rate_ms") == FIX_RATE_MS
    assert receiver.get("nav_pvt_rate") == 1
    assert receiver.get("running") is True
    assert receiver.get("resets") >= 2
    assert receiver.stats()["frame_errors"] == 0


@pytest.mark.parametrize("latitude,longitude,altitude,speed,heading,satellites,utc", FIXES)
def test_the_dut_driver_reports_the_injected_fix(dut, receiver, latitude, longitude, altitude, speed,
                                                 heading, satellites, utc):
    receiver.set(latitude=latitude, longitude=longitude, altitude=altitude, speed=speed,
                 heading=heading, satellites=satellites, fix=3, pdop=250, time=utc)
    before = gnss(dut)["fixes"]
    receiver.advance(FIX_RATE_MS)
    result = gnss(dut)
    assert result["fixes"] == before + 1
    assert result["fix_status"] == FIX_STATUS_GNSS_FIX
    assert result["fix_quality"] == FIX_QUALITY_GNSS_SPS
    assert result["latitude_ndeg"] == latitude * 100
    assert result["longitude_ndeg"] == longitude * 100
    assert result["altitude_mm"] == altitude
    assert result["speed_mmps"] == speed
    assert result["bearing_mdeg"] == heading // 100
    assert result["satellites"] == satellites
    assert result["hdop"] == 2500
    year, month, day, hour, minute, second = utc
    assert result["utc"] == [year % 100, month, day, hour, minute, second * 1000]


def test_no_fix_is_reported_as_no_fix(dut, receiver):
    receiver.set(latitude=473977420, longitude=-1223321500, fix=0)
    receiver.advance(FIX_RATE_MS)
    result = gnss(dut)
    assert result["fix_status"] == FIX_STATUS_NO_FIX
    assert result["fix_quality"] == FIX_QUALITY_INVALID


def test_one_fix_per_interval(dut, receiver):
    receiver.set(fix=3)
    receiver.advance(FIX_RATE_MS)
    before = gnss(dut)["fixes"]
    bytes_before = receiver.stats()["bytes_tx"]
    receiver.advance(FIX_RATE_MS // 2)
    assert gnss(dut)["fixes"] == before
    receiver.advance(FIX_RATE_MS // 2)
    assert gnss(dut)["fixes"] == before + 1
    receiver.advance(FIX_RATE_MS * 5)
    assert gnss(dut)["fixes"] == before + 2
    assert receiver.stats()["bytes_tx"] == bytes_before + 2 * 100


def test_the_pull_api_polls_the_part(dut, receiver):
    assert gnss(dut, "RATE")["fix_rate_ms"] == FIX_RATE_MS
    assert gnss(dut, "NAV")["navigation_mode"] == NAVIGATION_MODE_BALANCED
    systems = gnss(dut, "SYSTEMS")
    assert systems["supported"] == SYSTEM_GPS | SYSTEM_GLONASS | SYSTEM_GALILEO | SYSTEM_BEIDOU
    assert systems["enabled"] == SYSTEM_GPS | SYSTEM_GLONASS


def test_the_driver_only_comes_up_at_the_initial_baud(dut, receiver):
    receiver.set(baud=38400)
    dut.reset()
    dut.ping(timeout=20)
    assert gnss(dut)["status"] == "error"
    assert receiver.get("baud") == 38400
    receiver.reset()
    assert receiver.get("baud") == INITIAL_BAUD
    assert receiver.get("nav_pvt_rate") == 0
    dut.reset()
    dut.ping(timeout=20)
    assert gnss(dut)["status"] == "ok"
    assert receiver.get("baud") == CURRENT_SPEED
    assert receiver.get("nav_pvt_rate") == 1
    receiver.set(latitude=100000000, longitude=200000000, fix=3)
    receiver.advance(FIX_RATE_MS)
    result = gnss(dut)
    assert result["fixes"] == 1
    assert result["latitude_ndeg"] == 10000000000
