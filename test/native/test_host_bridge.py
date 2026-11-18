# SPDX-License-Identifier: Apache-2.0 OR MIT
# The host harness behind its bridge, with this test as the emulator: the
# models answer the DUT's I2C as they do on a bench, and their clock is the
# DUT's virtual time. Needs `just build-host`; skipped without it.

import pytest

from test.native.host import Bridge, HostHarness, default_build

PROGRAM = default_build()
pytestmark = pytest.mark.skipif(not PROGRAM.exists(), reason=f"no host harness at {PROGRAM} (just build-host)")

I2C0 = {"bus": 0, "sda": 4, "scl": 5}


@pytest.fixture
def bridged(tmp_path):
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    bridge.hello(i2c=[I2C0])
    yield harness, bridge
    bridge.close()
    harness.close()


def test_a_standalone_harness_lists_its_models():
    harness = HostHarness(PROGRAM)
    try:
        assert int(harness.run("import metalclaw\nprint(len(metalclaw.models()))")) > 50
    finally:
        harness.close()


def test_an_address_nothing_answers_is_not_acknowledged(bridged):
    _, bridge = bridged
    assert bridge.i2c(0, 0x18, write=b"\x06", read=2) is None


def test_the_mcp9808_answers_its_ids_on_the_dut_bus(bridged):
    # DS25095A registers 5-5 and 5-6: manufacturer 0054h, device 0400h, MSB
    # first, read after a pointer write.
    harness, bridge = bridged
    harness.run("import metalclaw\nt = metalclaw.attach('mcp9808', 't', i2c=(4, 5, 0x18))")
    assert bridge.i2c(0, 0x18, write=b"\x06", read=2) == b"\x00\x54"
    assert bridge.i2c(0, 0x18, write=b"\x07", read=2) == b"\x04\x00"
    # The pointer is kept: a read with no pointer write re-reads register 7.
    assert bridge.i2c(0, 0x18, read=2) == b"\x04\x00"


def test_a_dut_write_lands_in_the_model(bridged):
    # Register 2, T_UPPER, takes the word the DUT writes.
    harness, bridge = bridged
    harness.run("import metalclaw\nt = metalclaw.attach('mcp9808', 't', i2c=(4, 5, 0x18))")
    assert bridge.i2c(0, 0x18, write=b"\x02\x01\xe0") == b""
    assert bridge.i2c(0, 0x18, write=b"\x02", read=2) == b"\x01\xe0"


def test_the_runtime_clock_is_the_duts_virtual_time(bridged):
    harness, bridge = bridged
    harness.run("import metalclaw")
    before = int(harness.run("print(metalclaw.now_us())"))
    bridge.advance(250_000_000)
    after = int(harness.run("print(metalclaw.now_us())"))
    assert 249_000 <= after - before <= 251_000
    # Standing still in virtual time, it stands still however long we wait.
    import time
    time.sleep(0.2)
    assert int(harness.run("print(metalclaw.now_us())")) == after


def test_the_lp3943_takes_the_samples_led_writes(bridged):
    # zephyr/drivers/led/lp3943's first step, led_on(0): LS0 (06h) bits 1-0
    # to 01 (SNVS256E Table 6-8), as Zephyr's driver writes it, register then
    # value, after reading LS0 back.
    harness, bridge = bridged
    harness.run("import metalclaw\nleds = metalclaw.attach('lp3943', 'leds', i2c=(4, 5, 0x60))")
    assert bridge.i2c(0, 0x60, write=b"\x06", read=1) == b"\x00"
    assert bridge.i2c(0, 0x60, write=b"\x06\x01") == b""
    assert int(harness.run("print(leds.selectors)")) == 0x01
    # Input 1 (00h) reads the LED switched on as low.
    assert bridge.i2c(0, 0x60, write=b"\x00", read=1) == b"\xfe"


def test_a_gnss_streams_nmea_to_the_dut_at_its_baud_rate(tmp_path):
    # The DUT's UART1 on GP8 (TX) and GP9 (RX); the part sends on the DUT's
    # RX and listens on its TX. A NEO-M8N's NMEA starts at 9600 8N1: a byte
    # every 10 bit times.
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    try:
        bridge.hello(uart=[{"port": 1, "tx": 8, "rx": 9}])
        harness.run("import metalclaw\ngps = metalclaw.attach('neo_m8n', 'gps', uart=(9, 8))")
        for _ in range(1500):
            bridge.advance(1_000_000)
        sent = [a for a in bridge.actions if a["op"] == "uart"]
        text = bytes(a["data"] for a in sent).decode(errors="replace")
        assert all(a["port"] == 1 for a in sent)
        assert "$G" in text and "\r\n" in text, text[:200]
        gaps = {b["t_ns"] - a["t_ns"] for a, b in zip(sent, sent[1:])}
        assert min(gaps) >= 10 * 1_000_000_000 // 9600 - 1
    finally:
        bridge.close()
        harness.close()


def test_the_dut_s_bytes_reach_a_uart_model(tmp_path):
    # A UBX poll from the DUT (UBX-MON-VER, 0A 04, no payload) is answered.
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    try:
        bridge.hello(uart=[{"port": 1, "tx": 8, "rx": 9}])
        harness.run("import metalclaw\ngps = metalclaw.attach('neo_m8n', 'gps', uart=(9, 8))")
        poll = bytes([0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34])
        bridge.advance(1_000_000)
        bridge.actions.clear()
        bridge.request(op="uart", port=1, data=list(poll))
        for _ in range(300):
            bridge.advance(1_000_000)
        sent = bytes(a["data"] for a in bridge.actions if a["op"] == "uart")
        assert b"\xb5\x62\x0a\x04" in sent
    finally:
        bridge.close()
        harness.close()
