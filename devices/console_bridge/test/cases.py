# SPDX-License-Identifier: Apache-2.0 OR MIT

import random
import threading

import pytest
import serial

NAME = "dut_console"
IMAGE = "console_bridge"
INTERFACE = "Metalclaw DUT console"
BAUD = 115200
# A Meshtastic want_config answer is several KiB; this is a few of them.
BURST = 32 * 1024

DETACH = f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
"""

SELECT = f"""import metalclaw
bridge = [i for i in metalclaw.instances() if i.name == {NAME!r}][0]
"""


def attach(fixture):
    uart = fixture["uart"]
    pins = (uart["tx"], uart["rx"])
    return DETACH + f"metalclaw.attach('console_bridge', {NAME!r}, uart={pins})\n"


class Bridge:
    def __init__(self, harness, path):
        self.harness = harness
        self.path = path

    def get(self, name):
        return eval(self.harness.run(SELECT + f"print(bridge.{name})\n").strip())

    def set(self, **values):
        args = ", ".join(f"{k}={v!r}" for k, v in values.items())
        self.harness.run(SELECT + f"bridge.set({args})\n")

    def stats(self):
        return eval(self.harness.run(SELECT + "print(bridge.stats('uart'))\n").strip())

    def reset(self, factory=False):
        self.harness.run(SELECT + f"bridge.reset({factory!r})\n")

    def open(self, baud=BAUD, **options):
        return serial.Serial(self.path, baud, timeout=0.1, exclusive=True, **options)

    # What comes back of `payload` within `timeout` on the harness's clock.
    # Each clock read is a REPL round trip, so the clock is read only while
    # the port is idle, never while a burst flows.
    def round_trip(self, port, payload, timeout):
        port.reset_input_buffer()
        got = bytearray()

        def write():
            port.write(payload)
            port.flush()

        deadline = self.harness.clock() + timeout
        writer = threading.Thread(target=write)
        writer.start()
        while len(got) < len(payload):
            chunk = port.read(len(payload) - len(got))
            got += chunk
            if not chunk and self.harness.clock() >= deadline:
                break
        while writer.is_alive() and self.harness.clock() < deadline:
            writer.join(0.1)
        return bytes(got)


# Attaching adds a serial function to the harness's USB device, which
# re-enumerates it once; detaching does again. The DUT image echoes every
# byte its console UART receives, so what the host writes comes back.
@pytest.fixture(scope="module")
def bridge(harness, dut, fixture):
    dut.require_image(IMAGE)
    harness.run(attach(fixture))
    harness.wait_interface(INTERFACE)
    b = Bridge(harness, harness.serial_port(INTERFACE))
    yield b
    harness.run(DETACH)
    harness.wait_interface(INTERFACE, present=False)


@pytest.fixture
def fresh(bridge):
    bridge.reset(factory=True)
    return bridge


def line_seconds(n, baud=BAUD):
    return n * 10 / baud


def test_the_bridge_is_a_serial_port_named_for_the_console(harness, bridge):
    assert INTERFACE in harness.usb_interfaces()
    assert bridge.path.endswith("-if03"), bridge.path


def test_bytes_round_trip_through_the_dut(fresh):
    payload = bytes(range(256))
    with fresh.open() as port:
        assert fresh.round_trip(port, payload, 2.0) == payload
    assert fresh.get("from_host") == len(payload)
    assert fresh.get("to_host") == len(payload)


def test_a_long_burst_round_trips_without_loss(fresh):
    payload = random.Random(1).randbytes(BURST)
    before = fresh.stats()
    with fresh.open() as port:
        got = fresh.round_trip(port, payload, line_seconds(BURST) * 2 + 2)
    assert len(got) == len(payload)
    assert got == payload
    assert fresh.get("from_host") == BURST
    assert fresh.get("to_host") == BURST
    assert fresh.get("overruns") == 0
    after = fresh.stats()
    assert after["overruns"] == before["overruns"]
    assert after["frame_errors"] == before["frame_errors"]
    assert after["bytes_tx"] - before["bytes_tx"] == BURST
    assert after["bytes_rx"] - before["bytes_rx"] == BURST


# Closing waits until the host's bytes are in; most of the echo then comes
# back with the port closed and waits in the bridge's queue. Opening drops
# it: what the host may still see is what TinyUSB held, its FIFO and one
# armed packet.
def test_opening_the_port_drops_what_waited_for_a_host(fresh):
    payload = bytes(2000)
    with fresh.open() as port:
        port.write(payload)
    fresh.harness.sleep(0.3)
    assert fresh.get("to_host") == len(payload)
    with fresh.open() as port:
        stale = bytearray()
        deadline = fresh.harness.clock() + 0.3
        while fresh.harness.clock() < deadline:
            stale += port.read(4096)
        assert len(stale) <= 256 + 64, len(stale)
        assert fresh.round_trip(port, b"fresh", 1.0) == b"fresh"


def test_line_coding_is_ignored_unless_asked(fresh):
    with fresh.open() as port:
        port.baudrate = 9600
        fresh.harness.sleep(0.1)
        assert fresh.get("baud") == BAUD
        assert fresh.round_trip(port, b"still 115200", 1.0) == b"still 115200"


def test_line_coding_is_followed_when_asked(fresh):
    fresh.set(follow_line_coding=True)
    with fresh.open(57600) as port:
        fresh.harness.sleep(0.1)
        assert fresh.get("baud") == 57600
        assert abs(fresh.stats()["baud"] - 57600) < 57600 // 100
        port.baudrate = BAUD
        fresh.harness.sleep(0.1)
        assert fresh.get("baud") == BAUD
        assert fresh.round_trip(port, b"back at 115200", 1.0) == b"back at 115200"
        refused = fresh.get("codings_refused")
        port.parity = serial.PARITY_MARK
        fresh.harness.sleep(0.1)
        assert fresh.get("codings_refused") == refused + 1
        port.parity = serial.PARITY_NONE
        fresh.harness.sleep(0.1)
        assert fresh.round_trip(port, b"8N1 again", 1.0) == b"8N1 again"
    fresh.reset(factory=True)
    assert fresh.get("follow_line_coding") is False


def test_dtr_and_rts_are_reported_and_reset_nothing(dut, fresh):
    boots = dut.command("ID")["boots"]
    with fresh.open() as port:
        fresh.harness.sleep(0.1)
        assert fresh.get("dtr") is True
        port.dtr = False
        port.rts = False
        fresh.harness.sleep(0.1)
        assert fresh.get("dtr") is False
        assert fresh.get("rts") is False
        port.rts = True
        fresh.harness.sleep(0.1)
        assert fresh.get("rts") is True
        assert fresh.round_trip(port, b"no reset", 1.0) == b"no reset"
    assert dut.command("ID")["boots"] == boots
    assert dut.ping()["status"] == "ok"
