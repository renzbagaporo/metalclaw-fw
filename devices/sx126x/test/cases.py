# SPDX-License-Identifier: Apache-2.0 OR MIT

import time

import pytest

NAME = "radio"
DRIVER_IMAGES = ("sx126x", "sx126x/native")
RAW_IMAGE = "sx126x/raw"

EAGAIN = 11
EIO = 5

SYNC_PRIVATE = 0x1424
SYNC_PUBLIC = 0x3444
STATUS_STBY_RC = 0xA2
VERSION = b"SX1261 V2D 2D02\0"

TX_KEYS = ("length", "frequency", "packet_type", "sf", "bw", "cr", "ldro", "preamble", "implicit_header",
           "crc", "iq_inverted", "sync_word", "power", "start_ms", "airtime_us")

# freq Hz, bw kHz, sf, cr denominator, power dBm, preamble, public network
CONFIGS = [
    (869525000, 250, 9, 7, 14, 12, 0),
    (915000000, 125, 12, 5, 20, 8, 1),
]

# TxDone is checked on the worker's millisecond tick, which waits while the
# script or the probe holds the runtime lock.
TX_DONE_SLACK_US = 20_000
# The SetTx trace entry is stamped once the command has been handled.
TRACE_SLACK_US = 200

CLOCKS_HZ = [1_000_000, 2_000_000, 4_000_000, 8_000_000]
WINDOWS_PER_CLOCK = 1000
# The model answers each window on the worker before it lowers BUSY; a RAW
# burst does not wait for BUSY, so the gap keeps it from outrunning the model.
GAP_US = 300


def lora_airtime_us(sf, bw_hz, cr, preamble, length, crc=True, implicit=False, ldro=False):
    """DS_SX1261-2 6.1.4, rounded the way RadioLib's getTimeOnAir does."""
    bits = max(8 * length + (16 if crc else 0) - 4 * sf + (0 if sf < 7 else 8) + (0 if implicit else 20), 0)
    divisor = 4 * (sf - 2 if ldro else sf)
    blocks = -(-bits // divisor)
    quarters = (preamble + 8) * 4 + (25 if sf < 7 else 17) + blocks * (cr + 4) * 4
    return quarters * (1_000_000 << sf) // (4 * bw_hz)


def attach(fixture, nreset=True):
    spi = fixture["spi"]
    lines = fixture["gpio"]["sx126x"]
    pins = (spi["sck"], spi["mosi"], spi["miso"], spi["cs"]["sx126x"])
    reset = f", nreset={lines['nreset']}" if nreset else ""
    return f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
metalclaw.attach('sx1262', {NAME!r}, spi={pins}, busy={lines['busy']}, dio1={lines['dio1']}{reset})
"""


SELECT = f"""import metalclaw
radio = [i for i in metalclaw.instances() if i.name == {NAME!r}][0]
"""


class Radio:
    def __init__(self, harness):
        self.harness = harness

    def run(self, code):
        return self.harness.run(SELECT + code)

    def get(self, name):
        return eval(self.run(f"print(radio.{name})\n").strip())

    def stats(self):
        return eval(self.run("print(radio.stats('spi'))\n").strip())

    def trace(self):
        return eval(self.run("print(radio.trace())\n").strip())

    def inject(self, payload, fault=None, **params):
        code = ""
        if fault:
            code += f"radio.inject({fault!r})\n"
        if params:
            code += "radio.set(" + ", ".join(f"rx_{k}={v!r}" for k, v in params.items()) + ")\n"
        self.run(code + f"radio.write_region('rx', 0, {bytes(payload)!r})\n")

    def frames(self):
        """Every captured frame, oldest first, as (payload, metadata); empties the queue."""
        text = self.run(f"""out = []
while radio.tx_pending:
    out.append((radio.read_region('tx', 0, radio.tx_length), {{k: radio.get('tx_' + k) for k in {TX_KEYS!r}}}))
    radio.tx_next = True
print(out)
""")
        return eval(text.strip())


def lora(dut, line, timeout=10.0):
    result = dut.command(f"LORA {line}", timeout)
    assert result["test"] == "test_lora", result
    return result


def receive(dut, radio, timeout_ms, inject=None):
    """LORA RECV on the DUT; `inject` (payload, kwargs) goes on air once the DUT listens."""
    sequence = dut.start(f"LORA RECV {timeout_ms}")
    if inject is not None:
        time.sleep(0.3)
        payload, params = inject
        radio.inject(payload, **params)
    return dut.finish(sequence, timeout_ms / 1000 + 10)


@pytest.fixture(scope="module")
def radio(harness, dut, fixture):
    if dut.image_name() not in DRIVER_IMAGES:
        pytest.skip("the DUT image has no sx126x driver")
    harness.run(attach(fixture))
    dut.reset()
    dut.ping()
    return Radio(harness)


@pytest.fixture(scope="module")
def raw(harness, dut, fixture):
    dut.require_image(RAW_IMAGE)
    # The raw image leaves NRESET undriven, so it is not bound.
    harness.run(attach(fixture, nreset=False))
    # Out of reset the part reports a failed command until its first one.
    raw_window(dut, "8000", write=True)
    return Radio(harness)


def configure(dut, freq, bw, sf, cr, power, preamble, public, tx):
    result = lora(dut, f"CONFIG freq={freq} bw={bw} sf={sf} cr={cr} power={power} preamble={preamble} "
                       f"public={public} tx={int(tx)} iq=0 crc=1")
    assert result["status"] == "ok", result


def test_the_driver_brings_the_radio_up(radio):
    assert radio.get("resets") >= 1
    assert radio.get("commands") > 0
    assert radio.get("command_errors") == 0
    assert radio.get("tcxo_mv") == 1800
    assert radio.get("dio2_rf_switch") is True
    assert radio.get("mode") == 0, "the driver leaves the radio in warm-start sleep"


@pytest.mark.parametrize("config", CONFIGS)
def test_the_configuration_lands_in_the_model(dut, radio, config):
    freq, bw, sf, cr, power, preamble, public = config
    configure(dut, *config, tx=True)
    # The native driver programs the packet parameters when it sends.
    assert lora(dut, "SEND 00")["status"] == "ok"
    radio.frames()
    assert abs(radio.get("frequency") - freq) <= 1
    assert radio.get("packet_type") == 1
    assert radio.get("bw") == bw * 1000
    assert radio.get("sf") == sf
    assert radio.get("cr") == cr - 4
    assert radio.get("ldro") is ((1 << sf) * 1000 // bw >= 16000)
    assert radio.get("preamble") == preamble
    assert radio.get("crc") is True
    assert radio.get("iq_inverted") is False
    assert radio.get("sync_word") == (SYNC_PUBLIC if public else SYNC_PRIVATE)
    assert radio.get("power") == power
    assert radio.get("command_errors") == 0


@pytest.mark.parametrize("config", CONFIGS)
@pytest.mark.parametrize("payload", [b"Hello", bytes(range(40))])
def test_a_sent_payload_is_captured_with_its_air_parameters(dut, radio, config, payload):
    freq, bw, sf, cr, power, preamble, public = config
    configure(dut, *config, tx=True)
    radio.frames()
    sent = lora(dut, f"SEND {payload.hex()}", timeout=20.0)
    assert sent["status"] == "ok", sent
    frames = radio.frames()
    assert len(frames) == 1, frames
    data, meta = frames[0]
    ldro = (1 << sf) * 1000 // bw >= 16000
    assert data == payload
    assert meta["length"] == len(payload)
    assert abs(meta["frequency"] - freq) <= 1
    assert (meta["packet_type"], meta["sf"], meta["bw"], meta["cr"]) == (1, sf, bw * 1000, cr - 4)
    assert (meta["ldro"], meta["preamble"], meta["implicit_header"], meta["crc"], meta["iq_inverted"]) == \
        (ldro, preamble, False, True, False)
    assert meta["sync_word"] == (SYNC_PUBLIC if public else SYNC_PRIVATE)
    assert meta["power"] == power
    assert meta["airtime_us"] == lora_airtime_us(sf, bw * 1000, cr - 4, preamble, len(payload), ldro=ldro)
    # TxDone follows SetTx by the air time, on the harness's clock (the DUT's
    # own uptime runs several percent slow on this fixture).
    trace = radio.trace()
    start = [t for t, kind, addr, _, _ in trace if kind == "command" and addr == 0x83][-1]
    done = [t for t, kind, addr, _, _ in trace if kind == "state" and addr & 0x0001 and t > start][0]
    assert meta["airtime_us"] - TRACE_SLACK_US <= done - start <= meta["airtime_us"] + TX_DONE_SLACK_US, \
        (start, done, meta)
    assert radio.get("tx_dropped") == 0


def test_a_full_length_payload_is_captured_exactly(dut, radio):
    configure(dut, *CONFIGS[0], tx=True)
    radio.frames()
    assert lora(dut, "SEND pattern=255", timeout=20.0)["status"] == "ok"
    frames = radio.frames()
    assert len(frames) == 1
    assert frames[0][0] == bytes((i * 7 + 1) & 0xFF for i in range(255))


@pytest.mark.parametrize("rssi, snr", [(-73, 7), (-110, -5)])
def test_an_injected_frame_is_received_with_its_signal(dut, radio, rssi, snr):
    configure(dut, *CONFIGS[0], tx=False)
    before = radio.get("rx_received")
    payload = b"from the harness " + str(rssi).encode()
    result = receive(dut, radio, 3000, (payload, {"rssi": rssi, "snr": snr}))
    assert result["status"] == "ok", result
    assert bytes.fromhex(result["data"]) == payload
    assert result["len"] == len(payload)
    assert (result["rssi"], result["snr"]) == (rssi, snr)
    assert radio.get("rx_received") == before + 1


@pytest.mark.parametrize("params", [{"frequency": CONFIGS[0][0] + 1_000_000}, {"sf": 10}, {"bw": 125000},
                                    {"sync_word": SYNC_PUBLIC}, {"iq_inverted": True}])
def test_a_frame_on_another_channel_is_not_received(dut, radio, params):
    configure(dut, *CONFIGS[0], tx=False)
    missed = radio.get("rx_missed")
    result = receive(dut, radio, 1500, (b"not for you", params))
    assert result["status"] == "error" and result["errno"] == EAGAIN, result
    assert radio.get("rx_missed") == missed + 1


def test_the_receive_times_out_with_nothing_on_air(dut, radio):
    configure(dut, *CONFIGS[0], tx=False)
    result = receive(dut, radio, 1000)
    assert result["status"] == "error" and result["errno"] == EAGAIN, result


def test_a_corrupt_frame_reports_a_crc_error(dut, radio):
    configure(dut, *CONFIGS[0], tx=False)
    result = receive(dut, radio, 3000, (b"corrupted", {}))
    assert result["status"] == "ok"
    radio.run("radio.inject('crc_error')\n")
    result = receive(dut, radio, 3000, (b"corrupted", {}))
    assert result["status"] == "error" and result["errno"] == EIO, result


def test_the_public_sync_word_selects_what_is_heard(dut, radio):
    freq, bw, sf, cr, power, preamble, _ = CONFIGS[0]
    configure(dut, freq, bw, sf, cr, power, preamble, 1, tx=False)
    assert radio.get("sync_word") == SYNC_PUBLIC
    assert receive(dut, radio, 3000, (b"public", {}))["status"] == "ok"
    result = receive(dut, radio, 1500, (b"private", {"sync_word": SYNC_PRIVATE}))
    assert result["errno"] == EAGAIN, result


def test_no_window_faulted(radio):
    stats = radio.stats()
    assert stats["dma_faults"] == 0 and stats["short_windows"] == 0, stats
    assert stats["max_clock_hz"] >= 8_000_000


def raw_window(dut, frame, hz=0, write=False, count=1):
    line = f"RAW {frame}" + (" w" if write else "") + (f" {hz}" if hz else "") + \
        (f" n={count} gap={GAP_US}" if count > 1 else "")
    result = dut.command(line, 60.0)
    assert result["test"] == "test_raw" and result["status"] == "ok", result
    return result


def test_the_version_string_reads_from_its_registers(dut, raw):
    rx = bytes.fromhex(raw_window(dut, "1d0320" + "00" * 17)["rx"])
    assert rx[:4] == bytes([STATUS_STBY_RC] * 4)
    assert rx[4:] == VERSION


def test_the_status_byte_is_clocked_on_the_header(dut, raw):
    # The part clocks its GetStatus response, 0x22 without the reserved bit 7,
    # on the second byte too; the engine clocks the status byte there.
    assert raw_window(dut, "c00000")["rx"] == "a2a222"
    assert raw_window(dut, "1100000000")["rx"] == "a2a2000000"


def test_the_part_reports_a_failed_command_after_power_on(dut, raw):
    raw.run("radio.reset()\n")
    assert raw_window(dut, "c00000")["rx"] == "aaaa2a"
    raw_window(dut, "8000", write=True)
    assert raw_window(dut, "c00000")["rx"] == "a2a222"


def test_the_wake_up_nop_leaves_the_status(dut, raw):
    raw_window(dut, "55", write=True)
    commands = raw.get("commands")
    assert raw_window(dut, "00")["rx"] == "a8"
    assert raw.get("commands") == commands
    assert raw_window(dut, "c00000")["rx"] == "a8a828"
    raw_window(dut, "8000", write=True)


@pytest.mark.parametrize("bad", ["55", "80", "8a07"])
def test_a_refused_command_shows_in_the_status_until_the_next_command(dut, raw, bad):
    errors = raw.get("command_errors")
    raw_window(dut, bad, write=True)
    assert raw_window(dut, "c00000")["rx"] == "a8a828"
    assert raw_window(dut, "c00000")["rx"] == "a8a828", "reads leave the command status"
    raw_window(dut, "8000", write=True)
    assert raw_window(dut, "c00000")["rx"] == "a2a222"
    assert raw.get("command_errors") == errors + 1


def test_register_writes_read_back(dut, raw):
    raw_window(dut, "0d074024b4", write=True)
    assert raw_window(dut, "1d0740000000")["rx"][8:] == "24b4"
    assert raw.get("sync_word") == 0x24B4
    raw_window(dut, "0d03200000", write=True)
    assert bytes.fromhex(raw_window(dut, "1d0320" + "00" * 17)["rx"])[4:] == VERSION


def test_the_buffer_wraps_at_256(dut, raw):
    raw_window(dut, "0efea1b2c3d4", write=True)
    assert raw_window(dut, "1efe00" + "00" * 4)["rx"][6:] == "a1b2c3d4"
    assert raw_window(dut, "1e0000" + "00" * 2)["rx"][6:] == "c3d4"


def test_a_transmission_raises_tx_done_after_its_air_time(dut, raw):
    raw.frames()
    for frame in ["8a01", "8b07040100", "8c000800050100", "080001000100000000", "0e0048656c6c6f", "8f0000",
                  "02ffff", "83000000"]:
        raw_window(dut, frame, write=True)
    time.sleep(0.2)
    # TxDone, with the command status "TX done" until the next command.
    assert raw_window(dut, "1200000000")["rx"] == "acac000100"
    assert raw_window(dut, "c00000")["rx"] == "acac2c"
    raw_window(dut, "8000", write=True)
    assert raw_window(dut, "c00000")["rx"] == "a2a222"
    frames = raw.frames()
    assert len(frames) == 1 and frames[0][0] == b"Hello"
    assert frames[0][1]["airtime_us"] == lora_airtime_us(7, 125000, 1, 8, 5)


@pytest.mark.parametrize("hz", CLOCKS_HZ)
def test_reads_are_exact_across_the_clock_range(dut, raw, hz):
    raw_window(dut, "8a01", write=True)
    raw_window(dut, "0e10" + "".join(f"{(i * 37 + 5) & 0xFF:02x}" for i in range(24)), write=True)
    before = raw.stats()["dma_faults"]
    version = raw_window(dut, "1d0320" + "00" * 17, hz, count=WINDOWS_PER_CLOCK)
    assert bytes.fromhex(version["rx"])[4:] == VERSION and version["mismatches"] == 0, version
    buffer = raw_window(dut, "1e1000" + "00" * 24, hz, count=WINDOWS_PER_CLOCK)
    assert buffer["rx"][6:] == "".join(f"{(i * 37 + 5) & 0xFF:02x}" for i in range(24)), buffer
    assert buffer["mismatches"] == 0, buffer
    response = raw_window(dut, "1100000000", hz, count=WINDOWS_PER_CLOCK)
    assert response["rx"] == "a2a2010000" and response["mismatches"] == 0, response
    stats = raw.stats()
    assert stats["dma_faults"] == before and stats["max_clock_hz"] >= hz, stats


def frf(hz):
    return f"{round(hz * (1 << 25) / 32_000_000):08x}"


def raw_lora(dut, raw, sf=7, freq=868_100_000):
    """LoRa at `freq`, SF `sf`, 125 kHz, CR 4/5, every IRQ on DIO1, cleared."""
    raw.frames()
    for frame in ["8a01", f"8b{sf:02x}040100", "8c000c00ff0100", "08ffffffff00000000", "8f0000", "02ffff",
                  "86" + frf(freq)]:
        raw_window(dut, frame, write=True)


def test_a_frame_received_raises_the_receive_irqs_in_order(dut, raw):
    raw_lora(dut, raw)
    raw_window(dut, "82ffffff", write=True)
    raw.inject(b"hello raw", rssi=-80, snr=6, signal_rssi=-82)
    time.sleep(0.3)
    # RX mode, "data available" through every read, PreambleDetected |
    # HeaderValid | RxDone.
    assert raw_window(dut, "1200000000")["rx"] == "d4d4001600"
    assert raw_window(dut, "13000000")["rx"] == "d4d40900"
    assert bytes.fromhex(raw_window(dut, "1e0000" + "00" * 9)["rx"])[3:] == b"hello raw"
    assert raw_window(dut, "1400000000")["rx"] == "d4d4a018a4"
    raw_window(dut, "800000", write=True)


def test_a_single_receive_times_out(dut, raw):
    raw_lora(dut, raw)
    raw_window(dut, "82000400", write=True)
    time.sleep(0.3)
    assert raw_window(dut, "1200000000")["rx"] == "a2a2020000", "Timeout, back in STBY_RC"


def test_channel_activity_detection_sees_a_frame_on_air(dut, raw):
    raw_lora(dut, raw, sf=12)
    raw_window(dut, "8802160a00000000", write=True)
    raw_window(dut, "c5", write=True)
    time.sleep(0.3)
    assert raw_window(dut, "1200000000")["rx"] == "a2a2008000", "CadDone alone with nothing on air"
    raw_window(dut, "02ffff", write=True)
    raw.inject(bytes(120))
    raw_window(dut, "c5", write=True)
    time.sleep(0.3)
    assert raw_window(dut, "1200000000")["rx"] == "a2a2018000", "CadDone | CadDetected"


def test_a_duty_cycled_receiver_hears_a_frame(dut, raw):
    raw_lora(dut, raw)
    raw_window(dut, "94000100000100", write=True)
    assert raw.get("mode") == 6
    raw.inject(b"sniffed")
    time.sleep(0.3)
    assert raw_window(dut, "1200000000")["rx"][4:] == "001600"
    assert raw.get("mode") == 1, "back in STBY_RC after the packet"
