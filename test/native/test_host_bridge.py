# SPDX-License-Identifier: Apache-2.0 OR MIT
# The host harness behind its bridge, with this test as the emulator: the
# models answer the DUT's I2C as they do on a bench, and their clock is the
# DUT's virtual time. Needs `just build-host`; skipped without it.

import os
import time

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


BIT_9600 = 1_000_000_000 // 9600


def _decode_8n1(edges, bit_ns):
    # Bytes from (t_ns, level) changes of a line idling high, read mid-bit.
    def level_at(t):
        level = 1
        for at, value in edges:
            if at > t:
                break
            level = value
        return level
    out, i = bytearray(), 0
    while i < len(edges):
        start, value = edges[i]
        if value != 0:
            i += 1
            continue
        out.append(sum(level_at(start + (1 + b) * bit_ns + bit_ns // 2) << b for b in range(8)))
        end = start + 10 * bit_ns
        while i < len(edges) and edges[i][0] < end - bit_ns // 2:
            i += 1
    return bytes(out)


def test_a_uart_model_on_pins_no_uart_block_has_goes_bit_by_bit(tmp_path):
    # A PIO program's UART (uart_pio, pico-examples pio uart_rx): the part's
    # bytes reach the DUT as levels on its RX pin, a bit every 1/9600 s.
    harness, bridge = _bridged(tmp_path)
    try:
        harness.run("import metalclaw\ngps = metalclaw.attach('neo_m8n', 'gps', uart=(9, 8))")
        for _ in range(1500):
            bridge.advance(1_000_000)
        assert {"t_ns": 0, "op": "watch", "pin": 8} in bridge.actions or any(
            a["op"] == "watch" and a["pin"] == 8 for a in bridge.actions)
        edges = [(a["t_ns"], a["level"]) for a in bridge.actions if a["op"] == "gpio" and a["pin"] == 9]
        assert not [a for a in bridge.actions if a["op"] == "uart"]
        text = _decode_8n1(edges[1:], BIT_9600).decode(errors="replace")
        assert "$G" in text and "\r\n" in text, text[:200]
    finally:
        bridge.close()
        harness.close()


def test_the_dut_s_bits_reach_a_uart_model_on_pins_no_uart_block_has(tmp_path):
    # The DUT's PIO sends a UBX poll (UBX-MON-VER) bit by bit on GP8, as
    # the edges the bridge reports; the part answers it.
    harness, bridge = _bridged(tmp_path)
    try:
        harness.run("import metalclaw\ngps = metalclaw.attach('neo_m8n', 'gps', uart=(9, 8))")
        bridge.advance(1_000_000)
        bridge.actions.clear()
        poll = bytes([0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34])
        t = bridge.t_ns
        bridge.request(op="gpio", pin=8, level=1, t_ns=t)
        for byte in poll:
            level = 1
            for bit, value in enumerate([0, *((byte >> b) & 1 for b in range(8)), 1]):
                if value != level:
                    bridge.request(op="gpio", pin=8, level=value, t_ns=t + bit * BIT_9600)
                    level = value
            t += 10 * BIT_9600
        bridge.t_ns = t
        for _ in range(300):
            bridge.advance(1_000_000)
        edges = [(a["t_ns"], a["level"]) for a in bridge.actions if a["op"] == "gpio" and a["pin"] == 9]
        assert b"\xb5\x62\x0a\x04" in _decode_8n1(edges, BIT_9600)
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
        assert {"op": "watch", "pin": 6} in [{k: v for k, v in a.items() if k != "t_ns"} for a in bridge.actions]
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


def test_a_quadrature_encoder_steps_its_pins_at_its_rate(tmp_path):
    # 1000 steps a second: A and B change in turn, a state every millisecond.
    harness, bridge = _bridged(tmp_path)
    try:
        harness.run("import metalclaw\nq = metalclaw.attach('quadrature_encoder', 'q', ab=(10, 11))\nq.rate = 1000")
        for _ in range(20):
            bridge.advance(1_000_000)
        edges = sorted((a["t_ns"], a["pin"]) for a in bridge.actions
                       if a["op"] == "gpio" and a["pin"] in (10, 11) and a["t_ns"] > 2_000_000)
        gaps = {b[0] - a[0] for a, b in zip(edges, edges[1:])}
        assert gaps == {1_000_000}, sorted(gaps)[:5]
        assert {p for _, p in edges} == {10, 11}
    finally:
        bridge.close()
        harness.close()


def test_a_jumper_joins_its_pins_inside_the_emulator(tmp_path):
    # pico-examples' uart, spi and i2c loopbacks: the emulator wires GP4 to
    # GP5 itself, so the DUT's levels need no round trip, and blocks routed
    # to them talk directly; detaching the jumper parts them.
    harness, bridge = _bridged(tmp_path)
    try:
        bridge.actions.clear()
        harness.run("import metalclaw\nj = metalclaw.attach('jumper', 'j', source=(4,), sink=(5,))")
        bridge.advance(1_000_000)
        assert [sorted(a["pins"]) for a in bridge.actions if a["op"] == "join"] == [[4, 5]]
        bridge.actions.clear()
        bridge.request(op="gpio", pin=4, level=0)
        assert [a for a in bridge.actions if a["op"] == "gpio"] == []
        harness.run("metalclaw.detach('j')")
        bridge.advance(1_000_000)
        assert [sorted(a["pins"]) for a in bridge.actions if a["op"] == "unjoin"] == [[4, 5]]
    finally:
        bridge.close()
        harness.close()


def test_an_spi_page_target_answers_the_dut_a_byte_a_frame(tmp_path):
    # pico-examples' spi_master: the DUT's PL022 raises CS between frames,
    # so each frame is a window; the target sends its page and keeps the
    # DUT's.
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    try:
        bridge.hello(spi=[SPI0])
        harness.run("import metalclaw\np = metalclaw.attach('spi_page_target', 'p', sck=(18,), mosi=(19,), "
                    "miso=(16,), cs=(17,))\np.write_region('miso', 0, bytes(range(10, 20)))")
        got = bytes(bridge.request(op="spi", bus=0, mosi=0x80 + i, bits=8)["miso"] for i in range(4))
        bridge.advance(2_000_000)
        assert got == bytes([10, 11, 12, 13])
        assert harness.run("print(bytes(p.read_region('mosi', 0, 4)).hex())") == "80818283"
    finally:
        bridge.close()
        harness.close()


def test_an_spi_page_controller_clocks_the_dut_and_keeps_its_answers(tmp_path):
    # pico-examples' spi_slave: the harness clocks the DUT's slave block, a
    # frame per window; each frame is an spi_clock action and comes back as
    # an spi_clocked request.
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    try:
        bridge.hello(spi=[SPI0])
        # The controller's MOSI is the DUT's RX pin (GP16), its MISO the
        # DUT's TX pin (GP19): the bus's names swapped.
        harness.run("import metalclaw\nc = metalclaw.attach('spi_page_controller', 'c', sck=(18,), mosi=(16,), "
                    "miso=(19,), cs=(17,))\nc.write_region('mosi', 0, bytes(range(256)))\nc.interval = 1\nc.pages = 1")
        clocks = []
        for _ in range(200):
            reply = bridge.advance(1_000_000)
            clocks += [a for a in reply.get("actions", []) if a["op"] == "spi_clock"]
            if len(clocks) >= 256:
                break
        assert [a["mosi"] for a in clocks[:256]] == list(range(256))
        for i, a in enumerate(clocks[:256]):
            bridge.request(op="spi_clocked", bus=0, mosi=a["mosi"], miso=255 - i, t_ns=max(a["t_ns"], bridge.t_ns))
        bridge.advance(20_000_000)
        assert harness.run("print(bytes(c.read_region('miso', 0, 4)).hex())") == "fffefdfc"
    finally:
        bridge.close()
        harness.close()


def test_a_command_typed_ahead_of_the_raw_repl_is_kept(tmp_path):
    # The raw REPL switches its terminal's mode after each command; a
    # command already waiting then (two sent in one write) still runs.
    harness = HostHarness(PROGRAM, None)
    try:
        port = harness.repl.port
        port.reset_input_buffer()
        port.write(b"print(1)\x04print(2)\x04")
        out = b""
        deadline = time.monotonic() + 10
        while out.count(b"\x04") < 4 and time.monotonic() < deadline:
            out += port.read(port.in_waiting or 1)
        assert out.count(b"OK") == 2, out
        assert b"1" in out and b"2" in out, out
    finally:
        harness.close()


def test_the_hello_pulls_every_dut_line_up(tmp_path):
    # The harness's board pulls each header line up (pio_onewire's and
    # dht's released lines read high); the Pico's own GP23-25 and GP29
    # (VSYS/3) are left alone.
    harness, bridge = _bridged(tmp_path)
    try:
        bridge.advance(1_000_000)
        pulled = sorted(a["pin"] for a in bridge.actions if a["op"] == "pull" and a["level"] == 1)
        assert pulled == [*range(23), 26, 27, 28]
    finally:
        bridge.close()
        harness.close()


def test_a_models_delay_does_not_shift_the_duts_edges(tmp_path):
    # accel_trig's MPU-6050 pulses INT for 50 us at each 1 kHz sample, a
    # delay made in the tick's round; an edge of the DUT's that a request
    # carries across that tick keeps its own time, so a bit-level UART
    # beside it at 115200, where 50 us is six bits, frames every byte.
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    try:
        bridge.hello(i2c=[{"bus": 0, "sda": 4, "scl": 5}])
        harness.run("import metalclaw\nimu = metalclaw.attach('mpu6050', 'imu', i2c=(4, 5, 0x68), int=(15,))\n"
                    "gps = metalclaw.attach('neo_m8n', 'gps', uart=(9, 8))\ngps.baud = 115200")
        # Awake, 1 kHz samples (DLPF on, divider 0), data-ready interrupt on.
        for reg, value in ((0x6B, 0x00), (0x1A, 0x01), (0x19, 0x00), (0x38, 0x01)):
            assert bridge.i2c(0, 0x68, bytes([reg, value])) is not None
        bridge.advance(5_000_000)
        bridge.actions.clear()
        bit_ns = 1_000_000_000 // 115200
        sent = bytes([0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34]) * 4
        t = bridge.t_ns + 300_000
        for byte in sent:
            level = 1
            for bit, value in enumerate([0, *((byte >> b) & 1 for b in range(8)), 1]):
                if value != level:
                    bridge.request(op="gpio", pin=8, level=value, t_ns=t + bit * bit_ns)
                    level = value
            t += 10 * bit_ns
        bridge.t_ns = t
        assert any(a["op"] == "gpio" and a["pin"] == 15 for a in bridge.actions), "the MPU-6050 never pulsed"
        bridge.advance(1_000_000)
        stats = eval(harness.run("print(gps.stats('uart'))"))
        assert (stats["bytes_rx"], stats["frame_errors"]) == (len(sent), 0), stats
    finally:
        bridge.close()
        harness.close()


def test_a_tmp116_s_data_ready_clears_on_read_and_waits_out_its_averaging(tmp_path):
    # TMP116 datasheet 7.4.1: Data_Ready (configuration bit 13) sets at each
    # result and clears when the configuration is read; a configuration
    # write starts over, the first result after the active conversion time
    # (eight averages: 124 ms).
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    try:
        bridge.hello(i2c=[{"bus": 0, "sda": 4, "scl": 5}])
        harness.run("import metalclaw\nt = metalclaw.attach('tmp116', 't', i2c=(4, 5, 0x48))")

        def config():
            return int.from_bytes(bridge.i2c(0, 0x48, b"\x01", read=2), "big")
        for _ in range(1100):
            bridge.advance(1_000_000)
        assert config() & 0x2000
        assert not config() & 0x2000
        bridge.i2c(0, 0x48, b"\x01\x02\x20")
        for _ in range(100):
            bridge.advance(1_000_000)
        assert not config() & 0x2000
        for _ in range(30):
            bridge.advance(1_000_000)
        assert config() & 0x2000
    finally:
        bridge.close()
        harness.close()


def test_a_glitch_shorter_than_half_a_bit_is_no_frame(tmp_path):
    # lcd_uart: as the DUT sets UART1 up, GP8 dips low for 200 ns, before
    # the route shows the UART there; a UART checks its start bit in the
    # middle and takes no byte from it.
    harness, bridge = _bridged(tmp_path)
    try:
        harness.run("import metalclaw\ngps = metalclaw.attach('neo_m8n', 'gps', uart=(9, 8))")
        bridge.advance(1_000_000)
        t = bridge.t_ns + 100_000
        bridge.request(op="gpio", pin=8, level=1, t_ns=t)
        bridge.request(op="gpio", pin=8, level=0, t_ns=t + 1_000)
        bridge.request(op="gpio", pin=8, level=1, t_ns=t + 1_200)
        bridge.t_ns = t + 10_000_000
        bridge.request(op="tick")
        stats = eval(harness.run("print(gps.stats('uart'))"))
        assert (stats["bytes_rx"], stats["frame_errors"]) == (0, 0), stats
    finally:
        bridge.close()
        harness.close()


def test_a_console_bridge_drops_what_came_before_its_port_opened(tmp_path):
    # As on a bench: the DUT's console bytes while no host has the port
    # open are dropped, then what follows reaches whoever opened it
    # (hello_otp's boot run stays out of the judged console).
    import os
    path, usb = tmp_path / "native.sock", tmp_path / "usb"
    harness = HostHarness(PROGRAM, path, usb_dir=usb)
    bridge = Bridge(path)
    bit = 1_000_000_000 // 115200

    def send(text):
        t = bridge.t_ns + 50_000
        for byte in text:
            level = 1
            for i, v in enumerate([0, *((byte >> k) & 1 for k in range(8)), 1]):
                if v != level:
                    bridge.request(op="gpio", pin=0, level=v, t_ns=t + i * bit)
                    level = v
            t += 10 * bit
        bridge.t_ns = t + 1_000_000
        for _ in range(5):
            bridge.advance(1_000_000)
    try:
        bridge.hello()
        harness.run("import metalclaw\nc = metalclaw.attach('console_bridge', 'c', uart=(1, 0))")
        bridge.advance(2_000_000)
        send(b"boot run")
        fd = os.open(os.path.realpath(usb / "Metalclaw DUT console"), os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            for _ in range(5):
                bridge.advance(1_000_000)
            send(b"judged")
            got = b""
            for _ in range(50):
                try:
                    got += os.read(fd, 100)
                except BlockingIOError:
                    pass
                if got.endswith(b"judged"):
                    break
                bridge.advance(1_000_000)
            assert got == b"judged"
            assert harness.run("print(c.dtr, c.discarded)") == "True 8"
        finally:
            os.close(fd)
    finally:
        bridge.close()
        harness.close()


def test_two_console_bridges_get_a_link_each(tmp_path):
    # Two bridges, one interface string: the second link is "<string> 2",
    # and detaching the first leaves the second's link in place.
    path, usb = tmp_path / "native.sock", tmp_path / "usb"
    harness = HostHarness(PROGRAM, path, usb_dir=usb)
    bridge = Bridge(path)
    try:
        bridge.hello()
        harness.run("import metalclaw\na = metalclaw.attach('console_bridge', 'a', uart=(1, 0))\n"
                    "b = metalclaw.attach('console_bridge', 'b', uart=(5, 4))")
        first, second = usb / "Metalclaw DUT console", usb / "Metalclaw DUT console 2"
        assert first.is_symlink() and second.is_symlink()
        assert os.readlink(first) != os.readlink(second)
        harness.run("a.detach()")
        assert not first.exists() and second.is_symlink()
    finally:
        bridge.close()
        harness.close()


def test_a_uart_attached_before_its_route_lets_go_of_the_dut_s_rx(tmp_path):
    # A part attached before the DUT routes its UART holds the DUT's RX
    # high, idle, as a line that carries bits; the route that follows
    # carries whole bytes, so the drive is released.
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    try:
        bridge.hello(uart=[{"port": 1, "tx": 8, "rx": 9}])
        bridge.request(op="route", uart=[])
        harness.run("import metalclaw\ngps = metalclaw.attach('neo_m8n', 'gps', uart=(9, 8))")
        bridge.advance(1_000_000)
        assert any(a["op"] == "gpio" and a["pin"] == 9 and a["level"] == 1 for a in bridge.actions)
        bridge.actions.clear()
        bridge.request(op="route", uart=[{"port": 1, "tx": 8, "rx": 9}])
        bridge.advance(1_000_000)
        assert any(a["op"] == "gpio" and a["pin"] == 9 and a["level"] == "z" for a in bridge.actions)
    finally:
        bridge.close()
        harness.close()
