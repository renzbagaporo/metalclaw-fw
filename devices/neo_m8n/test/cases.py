# SPDX-License-Identifier: Apache-2.0 OR MIT


import pytest

NAME = "gnss"
# Zephyr's u-blox driver on the DUT's UART0 on the crossed pair, or on its
# UART1 on pins only the harness's PIO UART can serve.
IMAGES = {"neo_m8n": "uart", "neo_m8n/uart1": "uart1"}
# Zephyr's generic NMEA driver on UART1, reading what the part sends from
# power-up.
NMEA_IMAGES = {"neo_m8n/nmea": "uart1"}

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


def attach(fixture, uart_name):
    uart = fixture[uart_name]
    pins = (uart["tx"], uart["rx"])
    return f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
metalclaw.attach('neo_m8n', {NAME!r}, uart={pins})
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
        self.harness.sleep(0.2)

    def pause(self):
        self.harness.run("import metalclaw\nmetalclaw.pause()\n")

    def resume(self):
        self.harness.run("import metalclaw\nmetalclaw.resume()\n")

    def reset(self):
        self.harness.run(SELECT + "gnss.reset()\n")


# The clock runs by default and the part sends a fix every interval on its
# own. The cases that count fixes pause it and step it by hand.
@pytest.fixture(scope="module")
def receiver(harness, dut, fixture):
    image = dut.image_name()
    if image not in IMAGES:
        pytest.skip(f"the DUT image is not built for neo_m8n ({image})")
    harness.run(attach(fixture, IMAGES[image]))
    r = Receiver(harness)
    r.pause()
    dut.reset()
    dut.ping()
    yield r
    r.resume()


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


# Fixes fall on a grid whose phase is wherever the clock stood when it was
# paused, so a half interval may or may not hold one; two halves hold one.
def test_one_fix_per_interval(dut, receiver):
    receiver.set(fix=3)
    receiver.advance(FIX_RATE_MS)
    before = gnss(dut)["fixes"]
    bytes_before = receiver.stats()["bytes_tx"]
    receiver.advance(FIX_RATE_MS // 2)
    assert gnss(dut)["fixes"] in (before, before + 1)
    receiver.advance(FIX_RATE_MS // 2)
    assert gnss(dut)["fixes"] == before + 1
    receiver.advance(FIX_RATE_MS * 5)
    assert gnss(dut)["fixes"] == before + 2
    assert receiver.stats()["bytes_tx"] == bytes_before + 2 * 100


def test_fixes_stream_on_their_own_while_the_clock_runs(dut, receiver):
    receiver.set(fix=3)
    before = gnss(dut)["fixes"]
    receiver.resume()
    try:
        receiver.harness.sleep(2.5)
    finally:
        receiver.pause()
    assert gnss(dut)["fixes"] >= before + 2


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


# NMEA as the part sends it from power-up, read by Zephyr's gnss-nmea-generic
# driver: it publishes a fix when a GGA and an RMC carry the same UTC, and
# takes a UTC of 00:00:00.000 for none, so no epoch here falls on midnight.
NMEA_FIXES = [
    (473977420, -1223321500, 52000, 1500, 9000000, 9, (2026, 9, 13, 5, 42, 7)),
    (-338688200, 1512092900, -3000, 0, 0, 4, (2026, 1, 31, 23, 59, 50)),
    (900000000, 1800000000, 8848860, 250000, 35999999, 32, (2027, 1, 1, 0, 0, 1)),
    (0, 0, 0, 0, 27000000, 12, (2000, 2, 29, 12, 30, 0)),
]
NMEA_HDOP = 150
NMEA_STEP_S = 0.6


@pytest.fixture(scope="module")
def nmea_receiver(harness, dut, fixture):
    image = dut.image_name()
    if image not in NMEA_IMAGES:
        pytest.skip(f"the DUT image does not read NMEA ({image})")
    harness.run(attach(fixture, NMEA_IMAGES[image]))
    r = Receiver(harness)
    # The part's power-up epoch leaves at 9600 baud; a paused clock would hold
    # what is still queued and the DUT would see half an epoch.
    harness.sleep(NMEA_STEP_S)
    r.pause()
    dut.reset()
    dut.ping()
    yield r
    r.resume()


def nmea_step(receiver):
    # One epoch with the clock paused: RMC, VTG and GGA leave at once and
    # take about 200 ms at 9600 baud.
    receiver.advance(FIX_RATE_MS)
    receiver.harness.sleep(NMEA_STEP_S)


def test_nmea_is_on_from_power_up(nmea_receiver):
    assert nmea_receiver.get("baud") == INITIAL_BAUD
    assert nmea_receiver.get("protocols") == (7, 3)
    assert nmea_receiver.get("nmea_rates") == (1, 1, 1, 1, 1, 1)
    assert nmea_receiver.get("nmea_version") == 0x40
    assert nmea_receiver.get("gnss") == SYSTEM_GPS | SYSTEM_GLONASS
    assert nmea_receiver.get("nav_pvt_rate") == 0


@pytest.mark.parametrize("latitude,longitude,altitude,speed,heading,satellites,utc", NMEA_FIXES)
def test_the_dut_reads_the_injected_fix_from_nmea(dut, nmea_receiver, latitude, longitude, altitude, speed,
                                                  heading, satellites, utc):
    nmea_receiver.set(latitude=latitude, longitude=longitude, altitude=altitude, speed=speed,
                      heading=heading, satellites=satellites, fix=3, hdop=NMEA_HDOP, time=utc)
    before = gnss(dut)["fixes"]
    nmea_step(nmea_receiver)
    result = gnss(dut)
    assert result["fixes"] == before + 1
    assert result["fix_status"] == FIX_STATUS_GNSS_FIX
    assert result["fix_quality"] == FIX_QUALITY_GNSS_SPS
    # Minutes to 1e-5 in the sentence: within 84 nanodegrees.
    assert abs(result["latitude_ndeg"] - latitude * 100) <= 84
    assert abs(result["longitude_ndeg"] - longitude * 100) <= 84
    # Altitude to 0.1 m.
    assert abs(result["altitude_mm"] - altitude) <= 50
    # Knots to 0.001; Zephyr's knot is 9 ppm off.
    assert abs(result["speed_mmps"] - speed) <= 1 + speed // 50000
    # Course to 0.01 degree, 360.00 written as 0.00.
    assert result["bearing_mdeg"] == (heading + 500) // 1000 % 36000 * 10
    assert result["satellites"] == satellites
    assert result["hdop"] == NMEA_HDOP * 10
    year, month, day, hour, minute, second = utc
    assert result["utc"] == [year % 100, month, day, hour, minute, second * 1000]


def test_time_without_a_position_is_no_fix(dut, nmea_receiver):
    nmea_receiver.set(fix=5, time=(2026, 10, 3, 10, 0, 0))
    before = gnss(dut)["fixes"]
    nmea_step(nmea_receiver)
    result = gnss(dut)
    assert result["fixes"] == before + 1
    assert result["fix_status"] == FIX_STATUS_NO_FIX
    # No fix at all leaves the time out too: the driver has nothing to match.
    nmea_receiver.set(fix=0)
    nmea_step(nmea_receiver)
    assert gnss(dut)["fixes"] == before + 1


def test_nmea_streams_every_second_without_dropping(dut, nmea_receiver):
    nmea_receiver.set(latitude=100000000, longitude=200000000, satellites=12, fix=3,
                      time=(2026, 10, 3, 10, 0, 0))
    nmea_receiver.resume()
    try:
        nmea_receiver.harness.sleep(1.5)
        dropped = nmea_receiver.get("dropped")
        sentences = nmea_receiver.get("sentences")
        # The fix count is sampled somewhere inside each command, which takes
        # a good fraction of a second; the window is bounded by both.
        t0 = nmea_receiver.harness.clock()
        before = gnss(dut)["fixes"]
        t1 = nmea_receiver.harness.clock()
        nmea_receiver.harness.sleep(3.0)
        t2 = nmea_receiver.harness.clock()
        after = gnss(dut)["fixes"]
        t3 = nmea_receiver.harness.clock()
        assert nmea_receiver.get("dropped") == dropped
        # RMC, VTG, GGA, two GSA, two GPGSV, two GLGSV, GLL every second.
        assert nmea_receiver.get("sentences") - sentences >= 2 * 10
    finally:
        nmea_receiver.pause()
    assert int(t2 - t1) - 1 <= after - before <= int(t3 - t0) + 1
    assert nmea_receiver.stats()["bytes_tx"] > 0
    assert gnss(dut)["latitude_ndeg"] == 10000000000
