# SPDX-License-Identifier: Apache-2.0 OR MIT

import datetime
import time
import zlib

import pytest

from .common import (CLOSED_PORT, EAGAIN, EALREADY, ECHO_PORT, ECHO_RING, ENOTCONN, FILE, FILE_NAME, FILE_PATH,
                     HOSTS, HTTP_PORT, REQUEST_SIZE, SMS_INBOX_SIZE, Modem, attach, deliver_pdu, echo, pattern)

MODEL = "sim7080g"
IMAGE = "cellular/sim7080g"

# What the model answers to AT+CGMI, +CGMM, +CGMR, +CGSN and +CCID.
MANUFACTURER = "SIMCOM INCORPORATED"
MODEL_NAME = "SIMCOM_SIM7080"
REVISION = "Revision:1951B16SIM7080"
IMEI = "869951030006302"
ICCID = "89882280666027595036"
# The serving cell the model reports in +CPSI? (TAC 0x0001).
CELL = {"mcc": 1, "mnc": 1, "tac": 1, "sci": 12345678, "pci": 123, "band": "EUTRAN-BAND20",
        "earfcn": 6290, "dlbw": 0, "ulbw": 0, "rsrq": -10, "rssnr": 13}

# enum sim7080_state, zephyr/drivers/modem/simcom-sim7080.h.
STATE_INIT, STATE_IDLE, STATE_NETWORKING, STATE_GNSS, STATE_OFF = range(5)
# enum sim7080_ue_sys_mode and sim7080_ue_op_mode.
SYS_MODE_NO_SERVICE, SYS_MODE_LTE_NB_IOT = 0, 3
OP_MODE_ONLINE = 0

INIT_TIMEOUT_S = 90.0
POWER_TIMEOUT_S = 30.0
NET_TIMEOUT_S = 30.0
URC_SETTLE_S = 0.5


def mdm(dut, operation, timeout=15.0):
    result = dut.command(f"MODEM {operation}", timeout)
    assert result["test"] == "test_modem", result
    return result


def ensure_networking(dut, modem):
    # Brings the driver back to NETWORKING with PDP active from wherever a
    # case left it. Power off and on is the one path that works from every
    # state: stop_network() needs NETWORKING and an answered deactivation.
    state = mdm(dut, "STATE")["state"]
    if state == STATE_NETWORKING and modem.get("pdp"):
        return
    if state == STATE_GNSS:
        assert mdm(dut, "GNSS STOP")["rc"] == 0
        state = STATE_IDLE
    if state != STATE_IDLE:
        if state != STATE_OFF:
            mdm(dut, "POWER OFF", POWER_TIMEOUT_S)
        assert mdm(dut, "POWER ON", POWER_TIMEOUT_S)["rc"] == 0
    result = mdm(dut, "NET START", NET_TIMEOUT_S)
    assert result["rc"] == 0 and result["state"] == STATE_NETWORKING, result


@pytest.fixture(scope="module")
def modem(harness, dut, fixture):
    dut.require_image(IMAGE)
    harness.run(attach(fixture, MODEL))
    m = Modem(harness)
    m.load_network()
    dut.reset()
    dut.ping()
    m.before_init = {k: m.get(k) for k in ("powered", "power_cycles")}
    m.init = dut.command("MODEM INIT", INIT_TIMEOUT_S)
    m.after_init = {k: m.get(k) for k in ("powered", "power_cycles", "pdp", "unknown_commands")}
    yield m


@pytest.fixture
def networking(dut, modem):
    ensure_networking(dut, modem)
    return modem


def test_the_driver_boots_the_part_through_pwrkey(dut, modem):
    assert modem.before_init == {"powered": False, "power_cycles": 0}
    result = modem.init
    assert result["status"] == "ok", result
    assert result["rc"] == 0 and result["init_res"] == 0
    assert result["if_up"] == 0
    assert result["state"] == STATE_NETWORKING
    assert modem.after_init == {"powered": True, "power_cycles": 1, "pdp": True, "unknown_commands": 0}


def test_the_driver_reads_the_identity(dut, networking):
    info = mdm(dut, "INFO")
    assert info["status"] == "ok", info
    # The driver keeps 12 bytes of the manufacturer (MDM_MANUFACTURER_LENGTH,
    # sim7080.h) and strips "Revision:" (on_cmd_cgmr, sim7080.c).
    assert info["manufacturer"] == MANUFACTURER[:11]
    assert info["model"] == MODEL_NAME
    assert info["revision"] == REVISION.split(":", 1)[1]
    assert info["imei"] == IMEI
    assert info["iccid"] == ICCID


def test_pdp_is_up_and_the_interface_with_it(dut, networking):
    state = mdm(dut, "STATE")
    assert state["state"] == STATE_NETWORKING
    assert state["initialized"] is True and state["ready"] is True
    assert state["if_up"] is True
    assert networking.get("pdp") is True


@pytest.mark.parametrize("host", sorted(HOSTS))
def test_dns_resolves_the_hosts_region(dut, networking, host):
    result = dut.command(f"DNS {host}", 30.0)
    assert result["status"] == "ok", result
    assert result["ip"] == HOSTS[host]


def test_a_literal_address_resolves_to_itself(dut, networking):
    result = dut.command("DNS 192.0.2.17")
    assert result["status"] == "ok" and result["ip"] == "192.0.2.17", result


def test_an_unknown_name_resolves_to_nothing(dut, networking):
    # The model answers +CDNSGIP: 0,<err>; the driver logs it and still
    # returns success with 0.0.0.0 (on_cmd_cdnsgip, sim7080_dns.c).
    result = dut.command("DNS nowhere.test", 30.0)
    assert result["rc"] == 0 and result["ip"] == "0.0.0.0", result


@pytest.mark.parametrize("length", [1, 16, 255, 256, 512, 1000])
def test_tcp_echo_round_trips(dut, networking, length):
    before = {k: networking.get(k) for k in ("connections", "bytes_up", "bytes_down", "drops")}
    result = echo(dut, "TCP", "echo.test", f"len={length}", 3, ECHO_RING)
    assert result["status"] == "ok", result
    assert result["fd"] == 0
    assert result["rounds"] == 3 and result["mismatches"] == 0
    assert result["sent"] == result["received"] == 3 * length
    assert result["crc32"] == zlib.crc32(pattern(length))
    assert result["head"] == pattern(length)[:16].hex()
    assert networking.get("connections") == before["connections"] + 1
    assert networking.get("bytes_up") == before["bytes_up"] + 3 * length
    assert networking.get("bytes_down") == before["bytes_down"] + 3 * length
    assert networking.get("drops") == before["drops"]


def test_binary_bytes_survive_the_echo(dut, networking):
    payload = bytes([0x41, 0x00, 0x0D, 0x0A, 0xFF, 0x7E, 0x1B, 0x1A, 0x80, 0x3E])
    result = echo(dut, "TCP", HOSTS["echo.test"], payload.hex(), 2, ECHO_RING)
    assert result["status"] == "ok", result
    assert result["mismatches"] == 0
    assert result["crc32"] == zlib.crc32(payload)
    assert result["head"] == payload.hex()


@pytest.mark.parametrize("length", [1, 64, 256])
def test_udp_echo_round_trips(dut, networking, length):
    before = networking.get("drops")
    result = echo(dut, "UDP", "echo.test", f"len={length}", 3, length)
    assert result["status"] == "ok", result
    assert result["rounds"] == 3 and result["mismatches"] == 0
    assert result["sent"] == result["received"] == 3 * length
    assert result["crc32"] == zlib.crc32(pattern(length))
    assert networking.get("drops") == before


def test_http_get_returns_the_file_intact(dut, networking):
    result = dut.command(f"HTTP web.test {HTTP_PORT} {FILE_PATH}{FILE_NAME}", 60.0)
    assert result["status"] == "ok", result
    assert result["code"] == 200
    assert result["status_line"] == "HTTP/1.1 200 OK"
    assert result["content_length"] == len(FILE)
    assert result["body_length"] == len(FILE)
    assert result["body_crc32"] == zlib.crc32(FILE)
    assert networking.text_region("request", REQUEST_SIZE) == f"GET {FILE_PATH}{FILE_NAME} HTTP/1.1".encode()


def test_http_answers_404_without_a_file(dut, networking):
    networking.set(file_length=0)
    try:
        result = dut.command(f"HTTP web.test {HTTP_PORT} /missing", 60.0)
    finally:
        networking.set(file_length=len(FILE))
    assert result["status"] == "ok", result
    assert result["code"] == 404
    assert result["content_length"] == 0 and result["body_length"] == 0


def test_a_closed_port_refuses_the_connection(dut, networking):
    before = networking.get("connections")
    result = dut.command(f"TCP web.test {CLOSED_PORT} len=4", 30.0)
    assert result["status"] == "error" and result["stage"] == "connect", result
    # +CAOPEN: <cid>,<non-zero> becomes ENOTCONN (offload_connect, sim7080_sock.c).
    assert result["errno"] == ENOTCONN
    assert networking.get("connections") == before


def test_losing_registration_closes_the_socket_and_pdp(dut, networking):
    opened = dut.command(f"SOCK OPEN tcp echo.test {ECHO_PORT}", 30.0)
    assert opened["status"] == "ok", opened
    # The driver finds a socket for +CADATAIND and +CASTATE by fd, not by
    # connection id (modem_socket_from_fd in sim7080.c): the two must agree.
    assert opened["fd"] == 0
    try:
        assert dut.command("SOCK SEND 0 len=32", 30.0)["status"] == "ok"
        received = dut.command("SOCK RECV 0 32", 30.0)
        assert received["received"] == 32 and received["crc32"] == zlib.crc32(pattern(32)), received
        networking.set(registration=0)
        time.sleep(URC_SETTLE_S)
        assert networking.get("pdp") is False
        # +CASTATE: 0,0 marked the socket disconnected (sim7080_handle_sock_state),
        # so send fails before any AT traffic.
        refused = dut.command("SOCK SEND 0 len=8", 30.0)
        assert refused["status"] == "error" and refused["errno"] == ENOTCONN, refused
        # +APP PDP: 0,DEACTIVE only clears a flag; the state stays NETWORKING
        # (on_urc_app_pdp, sim7080.c).
        assert mdm(dut, "STATE")["state"] == STATE_NETWORKING
        # Without service +CPSI? has two fields, under the handler's minimum
        # of 14, so the struct is left as the DUT zeroed it and rc is 0.
        cell = mdm(dut, "CELL")
        assert cell["rc"] == 0 and cell["sys_mode"] == SYS_MODE_NO_SERVICE, cell
        reopened = dut.command(f"TCP {HOSTS['echo.test']} {ECHO_PORT} len=4", 30.0)
        assert reopened["stage"] == "connect" and reopened["errno"] == ENOTCONN, reopened
    finally:
        dut.command("SOCK CLOSE 0", 30.0)
        networking.set(registration=1)
    ensure_networking(dut, networking)
    assert echo(dut, "TCP", "echo.test", "len=64", 1, ECHO_RING)["status"] == "ok"


def model_decimal(value, places):
    sign = "-" if value < 0 else ""
    whole, fraction = divmod(abs(value), 10 ** places)
    return f"{sign}{whole}.{fraction:0{places}d}"


def driver_fixed(text, whole_scale, fraction):
    # gnss_split_on_dot and parse_cgnsinf, sim7080_gps.c: strtol on each side
    # of the dot, the fraction negated only when the whole part is negative,
    # so a value between -1 and 0 loses its sign.
    whole, frac = text.split(".")
    w, f = int(whole), int(frac)
    if w < 0:
        f = -f
    return w * whole_scale + fraction(f)


# latitude, longitude (1e-6 deg), altitude (mm), speed (0.01 km/h),
# course (0.1 deg), hdop (0.1), satellites, UTC
FIXES = [
    (47397742, -122332150, 52000, 1234, 1234, 13, 9, (2026, 9, 13, 5, 42, 7)),
    (-33868820, 151209290, -3500, 0, 0, 8, 4, (2026, 1, 31, 23, 59, 59)),
    (89999999, 179999999, 8848860, 99999, 3599, 99, 32, (2015, 1, 1, 0, 0, 0)),
    (-500000, -250000, -500, 5, 5, 10, 12, (2000, 2, 29, 12, 30, 0)),
]


def test_gnss_reports_injected_fixes(dut, networking):
    stop = mdm(dut, "NET STOP", NET_TIMEOUT_S)
    assert stop["rc"] == 0 and stop["state"] == STATE_IDLE, stop
    start = mdm(dut, "GNSS START")
    assert start["rc"] == 0 and start["state"] == STATE_GNSS, start
    assert networking.get("gnss_power") is True
    try:
        networking.set(fix=False)
        # run or fix not 1: the driver returns -EAGAIN (mdm_sim7080_query_gnss).
        assert mdm(dut, "GNSS QUERY")["rc"] == -EAGAIN
        for lat, lon, alt, speed, course, hdop, satellites, utc in FIXES:
            networking.set(latitude=lat, longitude=lon, altitude=alt, speed=speed, course=course,
                           hdop=hdop, satellites=satellites, time=list(utc), fix=True)
            result = mdm(dut, "GNSS QUERY")
            where = (lat, lon, alt)
            assert result["rc"] == 0 and result["run"] == 1 and result["fix"] == 1, (where, result)
            assert result["lat"] == driver_fixed(model_decimal(lat, 6), 10000000, lambda f: f * 10), where
            assert result["lon"] == driver_fixed(model_decimal(lon, 6), 10000000, lambda f: f * 10), where
            assert result["alt"] == driver_fixed(model_decimal(alt, 3), 1000, lambda f: f), where
            assert result["kmh"] == driver_fixed(model_decimal(speed, 2), 10, lambda f: int(f / 10)), where
            assert result["cog"] == course * 10, where
            assert result["hdop"] == hdop * 10, where
            assert result["utc"] == "%04d%02d%02d%02d%02d%02d.000" % utc, where
    finally:
        networking.set(fix=False)
        stopped = mdm(dut, "GNSS STOP")
    assert stopped["rc"] == 0 and stopped["state"] == STATE_IDLE, stopped
    assert networking.get("gnss_power") is False
    ensure_networking(dut, networking)


SMS = [
    ("+447700900123", "Hello from Metalclaw", (26, 10, 3, 12, 34, 56, 8), "+31624000000"),
    ("+15551234567", "@£$ 7-bit", (25, 12, 31, 23, 59, 58, 0), None),
    ("+819012345678", "The quick brown fox jumps over the lazy dog 0123456789 again!?", (26, 1, 1, 0, 0, 1, 36), None),
]


def test_sms_reads_and_deletes_injected_messages(dut, networking):
    networking.write_region("sms_inbox", b"\n".join(deliver_pdu(s, t, w, c) for s, t, w, c in SMS),
                            SMS_INBOX_SIZE)
    listed = mdm(dut, "SMS READ", 30.0)
    assert listed["rc"] == len(SMS) and listed["indices"] == [1, 2, 3], listed
    for position, (sender, text, stamp, _) in enumerate(SMS):
        message = mdm(dut, f"SMS READ {position}", 30.0)
        assert message["index"] == position + 1
        assert message["first_octet"] == 0x04 and message["pid"] == 0
        assert message["time"] == list(stamp)
        # The driver converts to UTF-8; the DUT escapes every byte above 0x7e.
        assert message["text"].encode("latin-1").decode("utf-8") == text, message
        assert message["length"] == len(text.encode("utf-8"))
    assert mdm(dut, "SMS DELETE 2")["rc"] == 0
    listed = mdm(dut, "SMS READ", 30.0)
    assert listed["rc"] == 2 and listed["indices"] == [1, 3], listed


def test_battery(dut, networking):
    networking.set(battery_mv=3712, battery_percent=63)
    result = mdm(dut, "BATTERY")
    assert result["rc"] == 0, result
    assert (result["bcs"], result["bcl"], result["mv"]) == (0, 63, 3712)


def test_cell_info(dut, networking):
    networking.set(rssi=17)
    try:
        result = mdm(dut, "CELL")
    finally:
        networking.set(rssi=20)
    assert result["rc"] == 0, result
    assert result["sys_mode"] == SYS_MODE_LTE_NB_IOT
    assert result["op_mode"] == OP_MODE_ONLINE
    assert {k: result[k] for k in CELL} == CELL
    rssi = -113 + 2 * 17
    assert (result["rssi"], result["rsrp"]) == (rssi, rssi - 10)
    # cpsi_parse_lte, sim7080_meas.c.
    assert result["sinr"] == 2 * result["rssnr"] - 20


# UTC, timezone in quarter hours: +CCLK? reports local time.
TIMES = [
    ((2026, 10, 3, 14, 5, 9), 0),
    ((2026, 10, 3, 14, 5, 9), 8),
    ((2026, 1, 1, 3, 0, 0), -20),
    ((2099, 12, 31, 23, 59, 59), 4),
]


@pytest.mark.parametrize("utc,timezone", TIMES)
def test_local_time(dut, networking, utc, timezone):
    networking.set(time=list(utc), timezone=timezone)
    result = mdm(dut, "TIME")
    assert result["rc"] == 0, result
    local = datetime.datetime(*utc) + datetime.timedelta(minutes=15 * timezone)
    # +CCLK? carries a two-digit year and the driver subtracts 1900 from it
    # (sim7080_utils_parse_time, sim7080_utils.c), so tm_year is yy - 1900.
    # The zone after the seconds is dropped.
    assert result["tm_year"] == local.year % 100 - 1900
    assert (result["tm_mon"], result["tm_mday"]) == (local.month - 1, local.day)
    assert (result["tm_hour"], result["tm_min"], result["tm_sec"]) == (local.hour, local.minute, local.second)
    assert result["tm_isdst"] == -1


def test_ftp_get_returns_the_file_intact(dut, networking):
    result = mdm(dut, f"FTP ftp.test {FILE_PATH} {FILE_NAME}", 60.0)
    assert result["rc"] == 0 and result["result"] == "finished", result
    assert result["bytes"] == len(FILE)
    assert result["crc32"] == zlib.crc32(FILE)
    assert networking.text_region("request", REQUEST_SIZE) == f"FTP {FILE_PATH}{FILE_NAME}".encode()


def test_power_off_and_on_through_pwrkey(dut, networking):
    cycles = networking.get("power_cycles")
    off = mdm(dut, "POWER OFF", POWER_TIMEOUT_S)
    assert off["rc"] == 0 and off["state"] == STATE_OFF, off
    assert networking.get("powered") is False
    # power_off() checks its own power flag first (mdm_sim7080_power_off).
    assert mdm(dut, "POWER OFF", POWER_TIMEOUT_S)["rc"] == -EALREADY
    on = mdm(dut, "POWER ON", POWER_TIMEOUT_S)
    assert on["rc"] == 0 and on["state"] == STATE_IDLE, on
    assert networking.get("powered") is True
    assert networking.get("power_cycles") == cycles + 1
    start = mdm(dut, "NET START", NET_TIMEOUT_S)
    assert start["rc"] == 0 and start["state"] == STATE_NETWORKING, start
    assert networking.get("pdp") is True
    assert echo(dut, "TCP", "echo.test", "len=64", 1, ECHO_RING)["status"] == "ok"


def test_a_dut_reset_reboots_the_modem(dut, networking):
    cycles = networking.get("power_cycles")
    dut.reset()
    dut.ping()
    state = mdm(dut, "STATE")
    assert state["initialized"] is False and state["state"] == STATE_INIT, state
    assert networking.get("powered") is True
    # The modem is still on, so the driver's first PWRKEY press turns it off
    # and the next one on again (modem_boot, sim7080.c).
    result = dut.command("MODEM INIT", INIT_TIMEOUT_S)
    assert result["rc"] == 0 and result["state"] == STATE_NETWORKING, result
    assert networking.get("power_cycles") == cycles + 1
    assert networking.get("pdp") is True
    assert echo(dut, "TCP", "echo.test", "len=64", 1, ECHO_RING)["fd"] == 0
