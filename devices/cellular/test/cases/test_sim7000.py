# SPDX-License-Identifier: Apache-2.0 OR MIT
# Zephyr has no SIM7000 driver, so these cases speak AT to the model through
# the DUT's UART, as TinyGSM's SIM7000 (CIP stack) and SIM7000SSL (CA stack)
# drivers do, and press PWRKEY through the DUT's GPIO.

import re
import time
import zlib

import pytest

from .common import (CLOSED_PORT, ECHO_PORT, ECHO_RING, FILE, FILE_NAME, FILE_PATH, HTTP_PORT, REQUEST_SIZE, Modem,
                     Uart, answer, attach, gpio, info_lines, pattern)

MODEL = "sim7000"
IMAGE = "cellular/sim7000"

MANUFACTURER = "SIMCOM_Ltd"
MODEL_NAME = "SIMCOM_SIM7000G"
REVISION = "Revision:1351B03SIM7000G"
PRODUCT = "SIM7000 R1351"
IMEI = "862818040001234"
IMSI = "001010123456789"
ICCID = "89882280666027595036"
APN = "internet"

# Longer than both Ton (1 s) and Toff (1.2 s): one press turns an off part
# on and an on part off.
PRESS_MS = 1500
# The press, boot_ms (4.5 s) and margin.
BOOT_MS = 15000
SMS_OUTBOX_SIZE = 192
# What one RECV can still report as text: the result line's room.
READ_CHUNK = 150


def press(dut):
    return gpio(dut, f"pwrkey PULSE {PRESS_MS}", PRESS_MS / 1000 + 5.0)


def boot(dut, uart):
    press(dut)
    return uart.until(BOOT_MS, "SMS Ready\r\n")


@pytest.fixture(scope="module")
def modem(harness, dut, fixture):
    dut.require_image(IMAGE)
    harness.run(attach(fixture, MODEL))
    m = Modem(harness)
    m.load_network()
    dut.reset()
    dut.ping()
    m.uart = Uart(dut)
    m.uart.flush()
    m.before_boot = {k: m.get(k) for k in ("powered", "power_cycles")}
    m.boot = boot(dut, m.uart)
    m.after_boot = {k: m.get(k) for k in ("powered", "power_cycles")}
    yield m


@pytest.fixture
def on(dut, modem):
    # The part on, echo off, and nothing stale in the DUT's ring.
    if not modem.get("powered"):
        boot(dut, modem.uart)
    modem.uart.flush()
    modem.uart.at("ATE0")
    return modem


def test_pwrkey_boots_the_part_with_its_urcs(dut, modem):
    assert modem.before_boot == {"powered": False, "power_cycles": 0}
    assert modem.after_boot == {"powered": True, "power_cycles": 1}
    assert info_lines(modem.boot) == ["RDY", "+CFUN: 1", "+CPIN: READY", "SMS Ready"]


def test_identity(dut, on):
    uart = on.uart
    assert info_lines(uart.at("ATI")) == [PRODUCT]
    assert info_lines(uart.at("AT+CGMI")) == [MANUFACTURER]
    assert info_lines(uart.at("AT+CGMM")) == [MODEL_NAME]
    assert info_lines(uart.at("AT+CGMR")) == [REVISION]
    assert info_lines(uart.at("AT+CGSN")) == [IMEI]
    assert info_lines(uart.at("AT+GSN")) == [IMEI]
    assert info_lines(uart.at("AT+CIMI")) == [IMSI]
    assert info_lines(uart.at("AT+CCID")) == [ICCID]
    assert info_lines(uart.at("AT+CPIN?")) == ["+CPIN: READY"]


def test_registration_and_signal(dut, on):
    uart = on.uart
    assert answer(uart.at("AT+CEREG?"), "+CEREG: ") == "0,1"
    on.set(rssi=17)
    try:
        assert answer(uart.at("AT+CSQ"), "+CSQ: ") == "17,99"
    finally:
        on.set(rssi=20)
    assert "OK" in uart.at("AT+CEREG=1")
    try:
        on.set(registration=0)
        assert uart.line(5000, "+CEREG: ") == "+CEREG: 0"
        assert answer(uart.at("AT+CEREG?"), "+CEREG: ") == "1,0"
        on.set(registration=5)
        assert uart.line(5000, "+CEREG: ") == "+CEREG: 5"
    finally:
        on.set(registration=1)
        uart.line(5000, "+CEREG: ")
        uart.at("AT+CEREG=0")


@pytest.fixture
def cip(dut, on):
    # TinyGSM's gprsConnect for the CIP stack, multi-IP and manual receive.
    uart = on.uart
    assert "SHUT OK" in uart.at("AT+CIPSHUT")
    for line in ("AT+CGATT=1", "AT+CIPMUX=1", "AT+CIPQSEND=1", "AT+CIPRXGET=1", f'AT+CSTT="{APN}","",""',
                 "AT+CIICR"):
        assert info_lines(uart.at(line)) == [], line
    on.ip_answer = info_lines(uart.at("AT+CIFSR;E0"))
    return on


def cip_open(uart, mux, host, port):
    assert info_lines(uart.at(f'AT+CIPSTART={mux},"TCP","{host}",{port}')) == []
    return uart.line(5000, f"{mux}, ")


def cip_send(uart, mux, data=None, payload=None, length=None):
    length = len(data) if data is not None else length
    assert uart.at(f"AT+CIPSEND={mux},{length}").endswith("> ")
    if data is not None:
        uart.tx(data)
    else:
        uart.send(payload)
    uart.until(5000, f"DATA ACCEPT:{mux},{length}\r\n")


def cip_read(uart, mux, want, crc=0):
    # +CIPRXGET: 2,<mux>,<n>,<rest>, then n raw bytes, then OK.
    uart.tx(f"AT+CIPRXGET=2,{mux},{want}\r".encode())
    head = uart.line(5000, f"+CIPRXGET: 2,{mux},")
    n, rest = (int(v) for v in head.split(",")[2:4])
    got = uart.recv(n, crc=crc) if n else {"received": 0, "crc32": crc, "data": ""}
    uart.until(2000, "OK\r\n")
    return n, rest, got


def test_cip_brings_the_bearer_up(dut, cip):
    assert cip.ip_answer == [".".join(str(v) for v in cip.get("ip"))]
    assert cip.get("pdp") is True
    # Manual 8.2.13: in multi-IP mode OK comes first, then the state.
    assert info_lines(cip.uart.at("AT+CIPSTATUS")) == []
    assert cip.uart.line(2000, "STATE: ") == "STATE: IP STATUS"


@pytest.mark.parametrize("length", [1, 64, 256])
def test_cip_tcp_echo(dut, cip, length):
    uart = cip.uart
    before = {k: cip.get(k) for k in ("connections", "bytes_up", "bytes_down", "drops")}
    assert cip_open(uart, 0, "echo.test", ECHO_PORT) == "0, CONNECT OK"
    cip_send(uart, 0, payload=f"len={length}", length=length)
    uart.line(5000, "+CIPRXGET: 1,0")
    assert answer(uart.at("AT+CIPRXGET=4,0"), "+CIPRXGET: ") == f"4,0,{length}"
    n, rest, got = cip_read(uart, 0, length)
    assert (n, rest) == (length, 0)
    assert got["crc32"] == zlib.crc32(pattern(length)), got
    assert answer(uart.at("AT+CIPCLOSE=0"), "0, ") == "CLOSE OK"
    assert cip.get("connections") == before["connections"] + 1
    assert cip.get("bytes_up") == before["bytes_up"] + length
    assert cip.get("bytes_down") == before["bytes_down"] + length
    assert cip.get("drops") == before["drops"]


def http_response(read):
    # Reads until the header and Content-Length bytes of body have come.
    data = b""
    for _ in range(60):
        n, rest, got = read()
        assert "data" in got, got
        data += got["data"].encode("latin-1")
        if b"\r\n\r\n" in data:
            header, body = data.split(b"\r\n\r\n", 1)
            length = re.search(rb"(?im)^content-length:\s*(\d+)", header)
            if length is not None and len(body) >= int(length.group(1)):
                return header, body
        if n == 0:
            time.sleep(0.1)
    raise AssertionError(f"incomplete response: {data!r}")


def test_cip_http_get_returns_the_file_intact(dut, cip):
    uart = cip.uart
    request = f"GET {FILE_PATH}{FILE_NAME} HTTP/1.1\r\nHost: web.test\r\n\r\n".encode()
    assert cip_open(uart, 0, "web.test", HTTP_PORT) == "0, CONNECT OK"
    cip_send(uart, 0, data=request)
    uart.line(5000, "+CIPRXGET: 1,0")
    header, body = http_response(lambda: cip_read(uart, 0, READ_CHUNK))
    assert header.split(b"\r\n")[0] == b"HTTP/1.1 200 OK"
    assert body == FILE and zlib.crc32(body) == zlib.crc32(FILE)
    assert cip.text_region("request", REQUEST_SIZE) == f"GET {FILE_PATH}{FILE_NAME} HTTP/1.1".encode()
    # The server closes once everything was read.
    assert uart.line(5000, "0, ") == "0, CLOSED"
    uart.at("AT+CIPCLOSE=0")


@pytest.mark.parametrize("host,port", [("nowhere.test", HTTP_PORT), ("web.test", CLOSED_PORT)])
def test_cip_connect_fails_on_an_unknown_name_or_a_closed_port(dut, cip, host, port):
    before = cip.get("connections")
    assert cip_open(cip.uart, 1, host, port) == "1, CONNECT FAIL"
    assert cip.get("connections") == before


@pytest.fixture
def ca(dut, on):
    # TinyGSM's SIM7000SSL gprsConnect: the application network through CNACT.
    uart = on.uart
    if answer(uart.at("AT+CNACT?"), "+CNACT: ").startswith("0,"):
        assert info_lines(uart.at(f'AT+CNACT=1,"{APN}"')) == []
        uart.line(5000, "+APP PDP: ")
    on.cnact = answer(uart.at("AT+CNACT?"), "+CNACT: ")
    return on


def ca_available(uart, cid):
    for line in info_lines(uart.at("AT+CARECV?")):
        if line.startswith(f"+CARECV: {cid},"):
            return int(line.split(",")[1])
    return 0


def ca_read(uart, cid, want, crc=0):
    # +CARECV: <n>,<n raw bytes>, then OK; no cid in front.
    uart.tx(f"AT+CARECV={cid},{want}\r".encode())
    uart.until(5000, "+CARECV: ")
    n = int(uart.until(1000, ",")[:-1].strip())
    got = uart.recv(n, crc=crc)
    uart.until(2000, "OK\r\n")
    return n, got


def test_ca_brings_the_application_network_up(dut, ca):
    assert ca.cnact == '1,"' + ".".join(str(v) for v in ca.get("ip")) + '"'
    assert ca.get("pdp") is True


@pytest.mark.parametrize("length", [1, 64, 256])
def test_ca_tcp_echo(dut, ca, length):
    uart = ca.uart
    before = {k: ca.get(k) for k in ("connections", "bytes_up", "bytes_down")}
    assert answer(uart.at(f'AT+CAOPEN=0,"echo.test",{ECHO_PORT}'), "+CAOPEN: ") == "0,0"
    assert uart.at(f"AT+CASEND=0,{length}").endswith("> ")
    uart.send(f"len={length}")
    # OK first, then the result (manual 18.2.5).
    sent = uart.until(5000, f"+CASEND: 0,0,{length}\r\n")
    assert sent.index("OK\r\n") < sent.index("+CASEND: "), sent
    uart.line(5000, "+CADATAIND: 0")
    n, got = ca_read(uart, 0, length)
    assert n == length and got["crc32"] == zlib.crc32(pattern(length)), got
    assert info_lines(uart.at("AT+CACLOSE=0")) == []
    assert ca.get("connections") == before["connections"] + 1
    assert ca.get("bytes_up") == before["bytes_up"] + length
    assert ca.get("bytes_down") == before["bytes_down"] + length


def test_ca_http_get_returns_the_file_intact(dut, ca):
    uart = ca.uart
    request = f"GET {FILE_PATH}{FILE_NAME} HTTP/1.1\r\nHost: web.test\r\n\r\n".encode()
    assert answer(uart.at(f'AT+CAOPEN=0,"web.test",{HTTP_PORT}'), "+CAOPEN: ") == "0,0"
    assert uart.at(f"AT+CASEND=0,{len(request)}").endswith("> ")
    uart.tx(request)
    uart.until(5000, f"+CASEND: 0,0,{len(request)}\r\n")
    uart.line(5000, "+CADATAIND: 0")

    def read():
        # A CARECV with nothing buffered is an error, so ask CARECV? first.
        if ca_available(uart, 0) == 0:
            return 0, 0, {"data": ""}
        n, got = ca_read(uart, 0, READ_CHUNK)
        return n, 0, got

    header, body = http_response(read)
    assert header.split(b"\r\n")[0] == b"HTTP/1.1 200 OK"
    assert body == FILE
    uart.at("AT+CACLOSE=0")


def test_ca_open_fails_on_an_unknown_name(dut, ca):
    before = ca.get("connections")
    # Manual 18.2.3 <result> 20: DNS failed.
    assert answer(ca.uart.at('AT+CAOPEN=1,"nowhere.test",80'), "+CAOPEN: ") == "1,20"
    assert ca.get("connections") == before


def model_decimal(value, places):
    sign = "-" if value < 0 else ""
    whole, fraction = divmod(abs(value), 10 ** places)
    return f"{sign}{whole}.{fraction:0{places}d}"


# latitude, longitude (1e-6 deg), altitude (mm), speed (0.01 km/h),
# course (0.1 deg), hdop (0.1), satellites, UTC
FIXES = [
    (47397742, -122332150, 52000, 1234, 1234, 13, 9, (2026, 9, 13, 5, 42, 7)),
    (-33868820, 151209290, -3500, 0, 0, 8, 4, (2026, 1, 31, 23, 59, 59)),
    (-500000, -250000, -500, 5, 5, 10, 12, (2000, 2, 29, 12, 30, 0)),
]


def test_gnss_reports_injected_fixes(dut, on):
    uart = on.uart
    assert answer(uart.at("AT+CGNSINF"), "+CGNSINF: ").split(",")[0] == "0"
    assert info_lines(uart.at("AT+CGNSPWR=1")) == []
    assert on.get("gnss_power") is True
    try:
        on.set(fix=False, satellites=3)
        fields = answer(uart.at("AT+CGNSINF"), "+CGNSINF: ").split(",")
        assert len(fields) == 21 and fields[:2] == ["1", "0"] and fields[14] == "3", fields
        for lat, lon, alt, speed, course, hdop, satellites, utc in FIXES:
            on.set(latitude=lat, longitude=lon, altitude=alt, speed=speed, course=course, hdop=hdop,
                   satellites=satellites, time=list(utc), fix=True)
            fields = answer(uart.at("AT+CGNSINF"), "+CGNSINF: ").split(",")
            # Manual table 15-1: run, fix, UTC, lat, lon, alt, speed, course,
            # fix mode, -, HDOP, PDOP, VDOP, -, in view, used, ...
            assert len(fields) == 21, fields
            assert fields[:3] == ["1", "1", "%04d%02d%02d%02d%02d%02d.000" % utc], fields
            assert fields[3:8] == [model_decimal(lat, 6), model_decimal(lon, 6), model_decimal(alt, 3),
                                   model_decimal(speed, 2), model_decimal(course, 1)], fields
            assert fields[8] == "1" and fields[10] == model_decimal(hdop, 1), fields
            assert fields[14:16] == [str(satellites), str(satellites)], fields
    finally:
        on.set(fix=False)
        uart.at("AT+CGNSPWR=0")
    assert on.get("gnss_power") is False


def test_sms_send(dut, on):
    uart = on.uart
    sent = on.get("sms_sent")
    assert info_lines(uart.at("AT+CMGF=1")) == []
    assert info_lines(uart.at('AT+CSCS="GSM"')) == []
    assert uart.at('AT+CMGS="+447700900123"').endswith("> ")
    uart.tx(b"Hello from Metalclaw\x1a")
    assert answer(uart.wait(10000), "+CMGS: ").isdigit()
    assert on.get("sms_sent") == sent + 1
    assert on.text_region("sms_outbox", SMS_OUTBOX_SIZE) == b"+447700900123\nHello from Metalclaw"


def test_cpowd_powers_down_with_its_message(dut, on):
    uart = on.uart
    uart.tx(b"AT+CPOWD=1\r")
    assert uart.line(5000, "NORMAL POWER DOWN") == "NORMAL POWER DOWN"
    assert on.wait_for("powered", False, 2.0) is False
    # Off, the part answers nothing.
    uart.tx(b"AT\r")
    assert uart.read(1000) == ""


def test_pwrkey_powers_off_and_on(dut, modem):
    uart = modem.uart
    if not modem.get("powered"):
        boot(dut, uart)
    cycles = modem.get("power_cycles")
    uart.flush()
    press(dut)
    assert uart.line(5000, "NORMAL POWER DOWN") == "NORMAL POWER DOWN"
    assert modem.get("powered") is False
    assert info_lines(boot(dut, uart)) == ["RDY", "+CFUN: 1", "+CPIN: READY", "SMS Ready"]
    assert modem.get("power_cycles") == cycles + 1
    # A boot starts with echo on.
    assert uart.at("AT") == "AT\r\r\nOK\r\n"
