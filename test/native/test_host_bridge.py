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


SPI0 = {"bus": 0, "sck": 18, "mosi": 19, "miso": 16}


def test_the_w25q_answers_and_programs_over_spi(tmp_path):
    # W25Q16JV datasheet 8.2: Read JEDEC ID (9Fh) EFh 40h 15h right after the
    # opcode; Write Enable (06h), Page Program (02h), Read Status (05h) busy
    # until tPP is over, then Read Data (03h) returns what was programmed.
    from test.native.host import spi_window
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    try:
        bridge.hello(spi=[SPI0])
        harness.run("import metalclaw\nf = metalclaw.attach('w25q16jv', 'flash', spi=(18, 19, 16, 17))\n"
                    "f.id_read = True\nf.held_base = 0xff000")
        assert spi_window(bridge, 0, 17, b"\x9f\x00\x00\x00")[1:] == b"\xef\x40\x15"
        spi_window(bridge, 0, 17, b"\x06")
        spi_window(bridge, 0, 17, b"\x02\x0f\xf0\x10" + b"\xa5\x5a")
        # BUSY from the program window's release, before the model ran it.
        assert spi_window(bridge, 0, 17, b"\x05\x00")[1] & 0x01
        bridge.advance(2_000_000)
        assert spi_window(bridge, 0, 17, b"\x05\x00")[1] & 0x01 == 0
        assert spi_window(bridge, 0, 17, b"\x03\x0f\xf0\x10\x00\x00")[4:] == b"\xa5\x5a"
    finally:
        bridge.close()
        harness.close()


def _bridged(tmp_path):
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    bridge.hello()
    return harness, bridge


def test_a_ws2812b_frame_from_the_dut_s_pulses(tmp_path):
    # The datasheet's windows as the model gives them: a 0 is a high pulse
    # of 250-550 ns, a 1 one of 650-950 ns, at 1.25 us a bit; a low of 50 us
    # ends the frame. One LED, G 10h, R 20h, B 30h.
    harness, bridge = _bridged(tmp_path)
    try:
        harness.run("import metalclaw\nstrip = metalclaw.attach('ws2812b', 'strip', din=(6,))\nstrip.count = 1")
        bridge.advance(1_000_000)
        assert {"op": "watch", "pin": 6} == {k: v for k, v in bridge.actions[-1].items() if k != "t_ns"}
        t = bridge.t_ns
        for byte in (0x10, 0x20, 0x30):
            for bit in range(7, -1, -1):
                high = 800 if byte >> bit & 1 else 400
                bridge.request(op="gpio", pin=6, level=1, t_ns=t)
                bridge.request(op="gpio", pin=6, level=0, t_ns=t + high)
                t += 1250
        bridge.t_ns = t + 100_000
        bridge.request(op="tick")
        for _ in range(3):
            bridge.advance(1_000_000)
        assert harness.run("print(strip.frames, strip.bad_frames)") == "1 0"
        assert harness.run("print(bytes(strip.read_region('leds', 0, 3)).hex())") == "102030"
    finally:
        bridge.close()
        harness.close()


def test_a_ds18b20_answers_a_reset_with_its_presence(tmp_path):
    # DS18B20 042208: a reset low of 480 us or more; the part waits 15-60 us
    # and pulls the line low for 60-240 us (the model: 30 us, then 120 us).
    harness, bridge = _bridged(tmp_path)
    try:
        harness.run("import metalclaw\nt = metalclaw.attach('ds18b20', 't', dq=(15,))")
        bridge.advance(1_000_000)
        start = bridge.t_ns
        bridge.request(op="gpio", pin=15, level=0, t_ns=start)
        bridge.actions.clear()
        bridge.request(op="gpio", pin=15, level="z", t_ns=start + 500_000)
        drive = [(a["t_ns"] - start, a["level"]) for a in bridge.actions if a["op"] == "gpio" and a["pin"] == 15]
        assert drive == [(530_000, 0), (650_000, "z")]
    finally:
        bridge.close()
        harness.close()


def test_a_route_report_moves_a_bus_to_where_the_dut_routed_it(bridged):
    # pico-examples' mpu9250_spi-style rerouting, on I2C: the hello says
    # GP4/GP5, the DUT moves I2C0 to GP8/GP9, and a part attached there
    # answers.
    harness, bridge = bridged
    harness.run("import metalclaw\nt = metalclaw.attach('mcp9808', 't', i2c=(8, 9, 0x18))")
    assert bridge.i2c(0, 0x18, write=b"\x06", read=2) is None
    bridge.request(op="route", i2c=[{"bus": 0, "sda": 8, "scl": 9}])
    assert bridge.i2c(0, 0x18, write=b"\x06", read=2) == b"\x00\x54"


def test_a_max7219_takes_the_dut_s_spi_writes(tmp_path):
    # MAX7219 datasheet Table 2: a 16-bit word, address then data, latched
    # at CS's rise; 0Ah is intensity.
    from test.native.host import spi_window
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    try:
        bridge.hello(spi=[SPI0])
        harness.run("import metalclaw\nm = metalclaw.attach('max7219', 'm', spi=(18, 19, 17))")
        bridge.advance(1_000_000)
        assert {"op": "watch", "pin": 17} in [{k: v for k, v in a.items() if k != "t_ns"} for a in bridge.actions]
        spi_window(bridge, 0, 17, b"\x0a\x05")
        bridge.advance(1_000_000)
        assert harness.run("print(m.intensity[0])") == "5"
    finally:
        bridge.close()
        harness.close()
