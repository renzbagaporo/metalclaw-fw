# SPDX-License-Identifier: Apache-2.0 OR MIT
# The wire part models built for the host with the firmware's warnings,
# behind stand-ins for the wire engines (wire_models_check.c), each fed
# what its engine would capture from the pico-examples program that uses it
# (pico-examples 0d62f75, the file and lines cited per program). What the
# model then holds must pass the checks parts.py makes on the bench, and
# what a program would make of the model's answers must pass its console
# criteria (part_targets.py).

import random
import struct
import subprocess
from pathlib import Path

import pytest

from test.unit.programs import lights
from test.unit.programs.part_targets import PART_SPECS
from test.unit.programs.parts import PARTS
from test.unit.test_part_models import strict_options

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).parent
DEVICES = ("ws2812b", "apa102", "ds18b20", "dht22", "quadrature_encoder", "tsop382", "hub75", "logic_capture",
           "pwm_capture", "sn754410")
SOURCES = ("core/src/onewire.c", "core/src/edges.c", "devices/ws2812b/ws2812b.c", "devices/apa102/apa102.c",
           "devices/ds18b20/ds18b20.c", "devices/dht22/dht22.c", "devices/quadrature_encoder/quadrature_encoder.c",
           "devices/tsop382/tsop382.c", "devices/tsop382/nec.c", "devices/hub75/hub75.c",
           "devices/logic_capture/logic_capture.c", "devices/pwm_capture/pwm_capture.c", "devices/sn754410/sn754410.c")
END = 0x100
BAD = 0x200
MARK = 0x80000000


@pytest.fixture(scope="module")
def check(tmp_path_factory):
    exe = tmp_path_factory.mktemp("wire_models") / "wire_models_check"
    includes = [arg for d in ("core/include", *(f"devices/{d}" for d in DEVICES)) for arg in ("-I", ROOT / d)]
    cmd = ["cc", "-O1", *strict_options(), "-fsanitize=undefined", "-fno-sanitize-recover=all", *includes,
           HERE / "wire_models_check.c", *(ROOT / s for s in SOURCES), "-o", exe]
    build = subprocess.run(cmd, capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    return exe


class Part:
    # What the harness REPL's instance gives a check expression, and what
    # the model gave its engines.
    def __init__(self, text: str):
        self._regions = {}
        self.lines = []
        self.pulses = []
        self.words = []
        for line in text.splitlines():
            kind, _, rest = line.partition(" ")
            if kind == "region":
                name, data = rest.split(" ")
                self._regions[name] = bytes.fromhex(data)
            elif kind == "prop":
                name, vtype, *values = rest.split(" ")
                got = tuple(bool(int(v)) if vtype == "bool" else int(v) for v in values)
                setattr(self, name, got[0] if len(got) == 1 else got)
            elif kind == "line":
                self.lines.append(rest)
            elif kind == "pulses":
                self.pulses = [int(v, 16) for v in rest.split()]
            elif kind == "words":
                self.words = [int(v, 16) for v in rest.split()]
            elif kind in ("period", "captures"):
                setattr(self, kind, int(rest))

    def read_region(self, name, offset, length):
        return self._regions[name][offset:offset + length]


def run(check, model, script, *args):
    got = subprocess.run([check, model, *args], input="\n".join(script) + "\n", capture_output=True, text=True,
                         timeout=120)
    assert got.returncode == 0, got.stdout[-2000:] + got.stderr
    return Part(got.stdout)


def judge(target, name, part):
    (attached,) = [a for a in PARTS[target] if a.name == name]
    return [c.description for c in attached.checks if not c.test(eval(c.expression, {"part": part}))]


def args_of(target, name):
    (attached,) = [a for a in PARTS[target] if a.name == name]
    return [f"{k}={','.join(map(str, v)) if isinstance(v, tuple) else v}" for k, v in attached.properties.items()]


def queue(binding, values):
    return f"q {binding} " + " ".join(f"{v:x}" for v in values)


# --- WS2812B

def test_ws2812b_holds_a_frame_the_ws2812_program_sent(check):
    script = []
    for frame in lights.ws2812_random_frames(12):
        script += [queue("din", [*frame, END]), "p"]
    part = run(check, "ws2812b", script, *args_of("pio_ws2812", "strip"))
    assert judge("pio_ws2812", "strip", part) == []
    assert part.frames == 12


def test_ws2812b_latches_nothing_from_a_spoiled_frame(check):
    good, other = list(lights.ws2812_random_frames(2))
    script = [queue("din", [*good, END]), queue("din", [*other[:30], BAD, *other[30:], END]), "p"]
    part = run(check, "ws2812b", script, "count=150")
    assert part.read_region("leds", 0, 450) == good
    assert (part.frames, part.bad_frames) == (1, 1)
    assert judge("pio_ws2812", "strip", part) != []


def test_ws2812b_passes_on_what_is_beyond_its_last_led(check):
    part = run(check, "ws2812b", [queue("din", [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, END]), "p"], "count=2", "channels=4")
    assert part.read_region("leds", 0, 12) == bytes((1, 2, 3, 4, 5, 6, 7, 8)) + bytes(4)
    assert part.length == 10


def test_ws2812b_strips_hold_frames_the_parallel_program_dithered(check):
    frames = list(lights.ws2812_parallel_frames(20))
    for strip, name in ((0, "strip0"), (1, "strip1")):
        script = []
        for frame in frames[5:20]:
            script += [queue("din", [*frame[strip], END]), "p"]
        part = run(check, "ws2812b", script, *args_of("pio_ws2812_parallel", name))
        assert judge("pio_ws2812_parallel", name, part) == []
        assert part.length == 256


def test_the_parallel_frames_are_the_programs_arithmetic():
    # ws2812_parallel.c:309-328: brightness starts at 0, so the first
    # frame is dark; strip 1's W bytes (every fourth) never light.
    frames = list(lights.ws2812_parallel_frames(300))
    assert frames[0] == (bytes(256), bytes(256))
    assert all(f[1][3::4] == bytes(64) for f in frames)
    assert all(f[0][192:] == bytes(64) for f in frames)
    assert max(max(f[0]) for f in frames) <= (255 * 0x40 >> 8) * 511 // 256 // 16 + 1
    assert any(any(f[1]) for f in frames)


# --- APA102

def apa102_bits(frames):
    bits = []
    for t in frames:
        words = [0] + [0xF0 << 24 | lights.WAVE[(3 * i + 4 * t) % 256] << 16 | lights.WAVE[(2 * i + 3 * t) % 256] << 8
                       | lights.WAVE[(i + t) % 256] for i in range(150)] + [0xFFFFFFFF]
        for w in words:
            bits += [(w >> (31 - b)) & 1 for b in range(32)]
    return bits


def pack(bits):
    bits = bits + [0] * (-len(bits) % 32)
    return [int("".join(map(str, bits[i:i + 32])), 2) for i in range(0, len(bits), 32)]


def test_apa102_aligns_on_a_start_frame_and_holds_the_programs_waves(check):
    # apa102.c:59-72; the capture starts mid-word, five bits into the stream.
    bits = [1, 0, 1, 1, 0] + apa102_bits(range(12))
    part = run(check, "apa102", [queue("spi", pack(bits)), "p"], *args_of("pio_apa102", "strip"))
    assert judge("pio_apa102", "strip", part) == []
    assert part.read_region("leds", 0, 600) == lights.apa102_frame(11)
    assert part.read_region("frame", 0, 600) == lights.apa102_frame(11)
    assert (part.frames, part.end_frames) == (12, 12)


def test_apa102_frame_holds_the_last_whole_update(check):
    # Steps 0-11 whole, then step 12 cut off after its 18th LED (start
    # frame and 18 LED frames): `leds` mixes 11 and 12, `frame` is 11.
    bits = apa102_bits(range(12)) + apa102_bits([12])[:32 * 19]
    part = run(check, "apa102", [queue("spi", pack(bits)), "p"], *args_of("pio_apa102", "strip"))
    assert part.read_region("frame", 0, 600) == lights.apa102_frame(11)
    assert not lights.apa102_shows_a_frame(part.read_region("leds", 0, 600))
    assert judge("pio_apa102", "strip", part) == []


def test_apa102_check_sees_a_wrong_led(check):
    bits = apa102_bits([7])
    bits[32 * 6 + 30] ^= 1
    part = run(check, "apa102", [queue("spi", pack(bits)), "p"], "count=150")
    assert not lights.apa102_shows_a_frame(part.read_region("leds", 0, 600))


# --- DS18B20, slot by slot as pio/onewire/onewire_library/onewire_library.c drives the bus

def write_slots(byte, bits=8):
    return "o " + "".join(str((byte >> i) & 1) for i in range(bits))


def read_slots(n=8):
    return "o " + "1" * n


def rom_bytes(rom):
    return list(rom.to_bytes(8, "little"))


def onewire_session(rom, conversions=2):
    # onewire.c:36-66 after ow_romsearch() (onewire_library.c:170-230) with
    # one device: the search command, then per bit two reads and the choice.
    script = ["o reset", write_slots(0xF0)]
    for i in range(64):
        script += [read_slots(2), f"o {(rom >> i) & 1}"]
    for _ in range(conversions):
        script += ["o reset", write_slots(0xCC), write_slots(0x44), read_slots(8), "t 760000", read_slots(8)]
        script += ["o reset", write_slots(0x55)] + [write_slots(b) for b in rom_bytes(rom)]
        script += [write_slots(0xBE), read_slots(8), read_slots(8)]
    return script


def program_text(part, rom):
    # What onewire.c prints from the lines it read.
    lines = iter(part.lines)
    next(lines)
    found = []
    for i in range(64):
        a, b = next(lines)
        found.append(a)
        assert b != a, f"search bit {i}: ({a}, {b})"
        next(lines)
    code = int("".join(reversed(found)), 2)
    text = f"Found 1 devices\n\t0: 0x{code:x}\n\n"
    while True:
        try:
            next(lines)
            next(lines)
            busy, done = next(lines), next(lines)
            # The part's answer to a slot is set at the slot before, so the
            # end of a conversion shows one slot late at most.
            assert busy == "00000000" and done in ("11111111", "01111111"), (busy, done)
            for _ in range(10):
                next(lines)
            lsb, msb = (int(next(lines)[::-1], 2) for _ in range(2))
        except StopIteration:
            return text
        temp = int.from_bytes(bytes((lsb, msb)), "little", signed=True)
        text += f"\t0: {temp / 16.0:f}\n"


def test_ds18b20_answers_the_onewire_programs_search_conversion_and_reads(check):
    rom = lights.ds18b20_rom(0xC0FFEE)
    part = run(check, "ds18b20", onewire_session(rom, conversions=3), "dq=15", *args_of("pio_onewire", "thermo"))
    text = program_text(part, rom)
    assert PART_SPECS["pio_onewire"].match(text) == ([], []), text
    assert judge("pio_onewire", "thermo", part) == []
    assert (part.rom[0] | part.rom[1] << 32) == rom


def test_ds18b20_sits_out_a_match_for_another_rom(check):
    rom = lights.ds18b20_rom(0xC0FFEE)
    other = lights.ds18b20_rom(0xC0FFEF)
    script = ["o reset", write_slots(0x55)] + [write_slots(b) for b in rom_bytes(other)]
    script += [write_slots(0xBE), read_slots(16)]
    part = run(check, "ds18b20", script, "serial=0xC0FFEE")
    assert part.lines[-1] == "1" * 16
    assert part.matches == 0


def test_ds18b20_scratchpad_carries_its_crc_and_written_resolution(check):
    # Write Scratchpad TH 30, TL -5, config 1Fh (9 bits): 93.75 ms (Table 2).
    script = ["o reset", write_slots(0xCC), write_slots(0x4E), write_slots(30), write_slots(0xFB),
              write_slots(0x1F), "p", "o reset", write_slots(0xCC), write_slots(0x44), "t 90000", read_slots(1),
              "t 5000", read_slots(1), read_slots(1), "o reset", write_slots(0xCC), write_slots(0xBE), read_slots(72)]
    part = run(check, "ds18b20", script, "temperature=-10125")
    pad = bytes(int(part.lines[-1][8 * i:8 * i + 8][::-1], 2) for i in range(9))
    assert part.lines[-6] == "0" and part.lines[-4] == "1"
    assert pad[2:5] == bytes((30, 0xFB, 0x1F))
    # -10.125 degC is FF5Eh at 12 bits (Table 1); 9 bits leave bits 2..0 undefined, 0 here.
    assert int.from_bytes(pad[0:2], "little", signed=True) == -162 & ~7
    crc = 0
    for byte in pad[:8]:
        for _ in range(8):
            mix = (crc ^ byte) & 1
            crc = (crc >> 1) ^ (0x8C if mix else 0)
            byte >>= 1
    assert crc == pad[8]
    assert part.resolution == 9


# --- DHT22, as gpio/dht_sensor/dht.c reads it

def line_levels(pulses):
    # The line from the host's release on, 1 us steps: released (high)
    # unless a pulse drives it low; high again after the last pulse.
    levels = []
    for p in pulses:
        levels += [0 if p & MARK else 1] * round((p & 0x7FFFFFFF) / 1000)
    return levels + [1] * 400


def _f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def dht_read(levels, step_us):
    # dht.c:44-91: counts each level in loops of sleep_us(1), `step_us` long
    # all told, a bit is 1 when its high level counted more than 16.
    data, last, j, t = [0] * 5, 1, 0, 0
    for i in range(85):
        count = 0
        while levels[min(t, len(levels) - 1)] == last:
            count += 1
            t += step_us
            if count == 255:
                break
        last = levels[min(t, len(levels) - 1)]
        if count == 255:
            break
        if i >= 4 and i % 2 == 0:
            data[j // 8] = (data[j // 8] << 1 | (1 if count > 16 else 0)) & 0xFF
            j += 1
    if j >= 40 and data[4] == sum(data[:4]) & 0xFF:
        f32 = _f32
        humidity = f32(((data[0] << 8) + data[1]) / 10)
        temp = f32((((data[2] & 0x7F) << 8) + data[3]) / 10)
        if data[2] & 0x80:
            temp = -temp
        return f"Humidity = {humidity:.1f}%, Temperature = {temp:.1f}C ({f32(f32(f32(temp * 9) / 5) + 32):.1f}F)\n"
    return "Bad data\n"


def test_dht22_waveform_reads_back_as_the_sensor_values(check):
    part = run(check, "dht22", [], *args_of("dht", "sensor"))
    assert len(part.pulses) == 84
    text = dht_read(line_levels(part.pulses), 2)
    assert PART_SPECS["dht"].match(text * 2) == ([], []), text


def test_dht22_frame_is_the_datasheets_example(check):
    # AM2302 data sheet: 65.2 %RH and 35.1 degC are 0000 0010 1000 1100,
    # 0000 0001 0101 1111, checksum 1110 1110; -10.1 degC is 1000 0000 0110 0101.
    for humidity, temp, frame in ((65200, 35100, "028c015fee"), (65200, -10100, "028c806573")):
        part = run(check, "dht22", [], f"humidity={humidity}", f"temperature={temp}")
        bits = "".join("1" if (p & 0x7FFFFFFF) > 50000 else "0" for p in part.pulses[4:83:2])
        assert f"{int(bits, 2):010x}" == frame


def test_dht_c_misreads_the_sensor_if_its_loop_takes_one_microsecond(check):
    # Its 16-count threshold sits between 0's 26-28 us and 1's 70 us only
    # when a sleep_us(1) pass takes about 2 us; at 1 us every bit reads 1.
    part = run(check, "dht22", [], *args_of("dht", "sensor"))
    assert dht_read(line_levels(part.pulses), 1) == "Bad data\n"


def test_dht22_answers_with_the_new_values_and_counts_responses(check):
    part = run(check, "dht22", ["r", "r", "p"], "humidity=1000", "temperature=-200")
    assert dht_read(line_levels(part.pulses), 2) == "Humidity = 1.0%, Temperature = -0.2C (31.6F)\n"
    assert part.responses == 2


# --- quadrature encoder

def states(words, count):
    return [(w >> (2 * s)) & 3 for w in words for s in range(16)][:count]


def test_quadrature_encoder_steps_forward_then_back_over_its_travel(check):
    part = run(check, "quadrature_encoder", ["p", "d 2", "p"], "travel=5", "rate=1000")
    assert part.period == 1000000
    seq = states(part.words, 20)
    # Forward is 00, 10, 11, 01 (B leads A), from position 0 (both low).
    assert seq == [2, 3, 1, 0, 2, 0, 1, 3, 2, 0, 2, 3, 1, 0, 2, 0, 1, 3, 2, 0]
    assert len(part.words) == 10
    assert part.steps == 160


def test_quadrature_encoder_waits_for_its_queue_before_a_new_rate(check):
    part = run(check, "quadrature_encoder", ["p", "q ab", "p"], "rate=-500")
    assert part.period == 2000000
    assert states(part.words, 4) == [1, 3, 2, 0]
    assert part.position == -128


# --- IR receiver

def nec_durations(address, data, stretch=30):
    # The IR path's view of a frame from pio/ir_nec/nec_transmit_library
    # (nec_carrier_control.pio): a 9 ms leader, 4.5 ms, 32 bits LSB first,
    # a closing burst; bursts come out stretched by the receiver.
    frame = address | (address ^ 0xFF) << 8 | data << 16 | (data ^ 0xFF) << 24
    out = [MARK | 9000 + stretch, 4500 - stretch]
    for i in range(32):
        out += [MARK | 562 + stretch, (1687 if frame >> i & 1 else 562) - stretch]
    return out + [MARK | 562 + stretch, 1000000]


def test_tsop38238_decodes_the_loopback_programs_frames(check):
    script = [queue("ir", nec_durations(0, 0)), queue("ir", nec_durations(0, 1)), "p"]
    part = run(check, "tsop38238", script)
    assert judge("pio_ir_loopback", "receiver", part) == []
    assert (part.frames, part.address, part.command, part.errors) == (2, 0, 1, 0)


def test_tsop38238_counts_a_broken_frame_and_recovers(check):
    broken = nec_durations(0, 7)
    del broken[20:24]
    part = run(check, "tsop38238", [queue("ir", broken + nec_durations(0x5A, 0x33)), "p"])
    assert (part.frames, part.address, part.command) == (1, 0x5A, 0x33)
    assert part.errors >= 1


# --- HUB75

HUB75_PINS = ("clock=0,1,2", "upper=4,5,6", "lower=8,9,10", "rows=12,13,14,15", "e=16")


def hub75_captures(rows, planes, enabled_ns=800, garbage=0):
    # pio/hub75/hub75.c:52-75 with hub75.pio: per row and bit plane, 128
    # pixel pairs and a dummy shifted (the first edge clocks garbage), a
    # latch with the row on A-E, then OEn low for 100 << bit system clocks,
    # 800 ns per 100 at the pico's 125 MHz. The stand-in's pins: clock 0,
    # latch 1, enable 2, R0 G0 B0 on 4-6, R1 G1 B1 on 8-10, A-D on 12-15, E 16.
    raw = "".join(lights.HUB75_THUMB_RGB565)
    pix = [int(raw[i:i + 4], 16) for i in range(0, len(raw), 4)]

    def gamma(p):
        return ((p & 0xF800) ** 2) >> 24 | ((((p & 0x07E0) ** 2) >> 14) & 0xFF) << 8 | ((((p & 0x1F) ** 2) >> 2) & 0xFF) << 16

    out = []
    for row in rows:
        for bit in planes:
            samples = [1 | garbage]
            for x in range(128):
                word = 1
                if x % 8 == 0:
                    for half, base in ((0, 4), (1, 8)):
                        rgb = gamma(pix[(row + 32 * half) * 16 + x // 8])
                        for ch in range(3):
                            word |= ((rgb >> (8 * ch + bit)) & 1) << (base + ch)
                samples.append(word)
            latch = sum(((row >> a) & 1) << (12 + a) for a in range(5))
            out.append(samples + [latch, enabled_ns << bit])
    return out


def test_hub75_shows_the_mountains_from_the_programs_latches(check):
    captures = hub75_captures(range(32), range(8))
    random.Random(7).shuffle(captures)
    script = [queue("clock", c) for c in captures] + ["p"] * len(captures)
    part = run(check, "hub75", script, *HUB75_PINS)
    assert judge("pio_hub75", "panel", part) == []
    assert (part.latches, part.rows_seen, part.unmatched, part.short) == (256, 0xFFFFFFFF, 0, 0)


def test_hub75_check_sees_a_missing_plane(check):
    captures = hub75_captures(range(32), range(7))
    part = run(check, "hub75", [queue("clock", c) for c in captures] + ["p"] * len(captures), *HUB75_PINS)
    assert judge("pio_hub75", "panel", part) != []


def test_hub75_drops_a_capture_cut_short(check):
    (capture,) = hub75_captures([3], [2])
    part = run(check, "hub75", [queue("clock", capture[:40] + capture[-2:]), "p"], *HUB75_PINS)
    assert (part.latches, part.short) == (0, 1)
    assert part.captures == 1


# --- EDGES_IN models: what the engine reads off a pin the DUT drives, a
# level's length in ns, MARK for a high (hal/wire.h)

def pwm(high_us, low_us, cycles):
    return [MARK | high_us * 1000, low_us * 1000] * cycles


def test_logic_capture_counts_edges_and_keeps_the_last_levels(check):
    part = run(check, "logic_capture", [queue("pin", [MARK | 500_000, 1_000_000, MARK | 250_000]), "l pin 0",
                                         "t 3000"])
    assert (part.rising, part.falling, part.edges, part.level, part.missed) == (1, 2, 3, 0, 0)
    assert (part.high_ns, part.low_ns, part.last_edge_ms) == (250_000, 1_000_000, 1)


def test_logic_capture_counts_an_overrun_and_starts_again_on_reset(check):
    part = run(check, "logic_capture", [queue("pin", [1000]), "l pin 1", "t 1000", "v pin", "t 1000"])
    assert (part.rising, part.missed, part.level) == (1, 1, 1)
    part = run(check, "logic_capture", [queue("pin", [1000, MARK | 1000]), "t 1000"])
    assert part.edges == 2


def test_pwm_capture_reads_period_and_pulse_and_follows_a_duty_change(check):
    # 1 kHz at 30 %, then 75 %: the change shows from the first cycle at
    # the new duty.
    part = run(check, "pwm_capture", [queue("pin", pwm(300, 700, 3)), "l pin 1", "t 1000"])
    assert (part.period_ns, part.pulse_ns, part.duty_ppm, part.still, part.periods) == (1_000_000, 300_000, 300_000,
                                                                                        False, 3)
    part = run(check, "pwm_capture", [queue("pin", pwm(300, 700, 3) + pwm(750, 250, 1)), "l pin 1", "t 1000"])
    assert (part.period_ns, part.pulse_ns, part.duty_ppm) == (1_000_000, 750_000, 750_000)


def test_pwm_capture_reads_a_held_line_as_0_or_100_percent(check):
    # No edge for more than two cycles (and the poll's slack): the duty is
    # the level's; the last period is kept.
    for level, duty in ((0, 0), (1, 1_000_000)):
        part = run(check, "pwm_capture", [queue("pin", pwm(300, 700, 2)), f"l pin {level}", "t 1000", "t 5000"])
        assert (part.still, part.duty_ppm, part.period_ns) == (True, duty, 1_000_000)
        assert part.pulse_ns == (1_000_000 if level else 0)
    part = run(check, "pwm_capture", ["l pin 1", "t 1000"])
    assert (part.still, part.duty_ppm, part.periods) == (True, 1_000_000, 0)


def test_pwm_capture_pairs_no_cycle_across_an_overrun_or_a_long_level(check):
    script = [queue("pin", [MARK | 300_000]), "t 1000", "v pin", queue("pin", [700_000]), "t 1000"]
    part = run(check, "pwm_capture", script)
    assert (part.periods, part.missed) == (0, 1)
    part = run(check, "pwm_capture", [queue("pin", [MARK | 0x7FFFFFFF, 700_000, MARK | 300_000, 0x7FFFFFFF]),
                                      "t 1000"])
    assert part.periods == 0


def test_sn754410_follows_the_function_table(check):
    # 9.4 Table 1: EN high passes A, EN low turns the output off; 3,4 not
    # wired read high (9.3.2).
    part = run(check, "sn754410", ["l a1 1", "l a2 0", "l en12 1", "t 1000"], "a3=-", "a4=-", "en34=-")
    assert part.outputs == (1, 0, 1, 1)
    assert part.drive_ppm == (1_000_000, 0)
    part = run(check, "sn754410", ["l a1 1", "l a2 0", "l en12 0", "t 1000"], "a3=-", "a4=-", "en34=-")
    assert part.outputs == (2, 2, 1, 1)
    assert part.drive_ppm[0] == 0


def test_sn754410_drive_from_pwm_on_enable_or_on_one_input(check):
    # Sign-magnitude on EN: forward at 40 %.
    part = run(check, "sn754410", [queue("en12", pwm(400, 600, 3)), "l en12 1", "l a1 1", "l a2 0", "t 1000"])
    assert part.duty_ppm[:3] == (1_000_000, 0, 400_000)
    assert part.drive_ppm[0] == 400_000
    assert part.period_ns[2] == 1_000_000
    # PWM on 2A at 25 %, 1A low, EN high: reverse.
    part = run(check, "sn754410", [queue("a2", pwm(250, 750, 3)), "l a2 1", "l a1 0", "l en12 1", "t 1000"])
    assert part.drive_ppm[0] == -250_000


def test_sn754410_drive_from_locked_anti_phase(check):
    # Complementary PWM on 1A (70 %) and 2A (30 %), EN high: 1Y over 2Y by
    # 40 % of VCC2 on average.
    script = [queue("a1", pwm(700, 300, 3)), queue("a2", pwm(300, 700, 3)), "l a1 1", "l a2 0", "l en12 1",
              "t 1000"]
    part = run(check, "sn754410", script)
    assert part.drive_ppm[0] == 400_000
    assert part.missed == 0
