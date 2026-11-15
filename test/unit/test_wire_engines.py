# SPDX-License-Identifier: Apache-2.0 OR MIT
# The wire engines' PIO programs, encoded by core/ports/rp2/hal/
# wire_programs.c for a harness clock, run in the PIO emulator (pio_sim.py)
# against the pico-examples programs' own PIO programs on the DUT's clock,
# on shared lines. The DUT programs are transcribed from pico-examples
# 0d62f75 (the .pio files named per test; BSD-3-Clause, Copyright (c)
# 2020-2023 Raspberry Pi (Trading) Ltd. and mjcross); their C drivers'
# FIFO traffic is played from Python. Where the firmware runs core code
# (the 1-Wire layer, the NEC decoder) the tests call the same C through a
# host library.

import ctypes
import random
import subprocess
from pathlib import Path

import pytest

from test.pico_examples import lights
from test.unit.pio_sim import NS, US, Sim, StateMachine, assemble
from test.unit.test_part_models import strict_options
from test.unit.test_wire_models import dht_read

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).parent
MS = 1000 * US
HARNESS_CLOCKS = (125_000_000, 150_000_000)
END = 0xFFFFFFFF
BAD = 0xFFFFFFFE


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    out = tmp_path_factory.mktemp("wire_engines")
    dump = out / "wire_programs_dump"
    cmd = ["cc", "-O1", *strict_options(), "-I", ROOT / "core/include", "-I", ROOT / "core/ports/rp2/hal",
           "-I", HERE / "pio_shim", ROOT / "core/ports/rp2/hal/wire_programs.c", HERE / "wire_programs_dump.c",
           "-o", dump]
    result = subprocess.run(cmd, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    lib = out / "wire_layers.so"
    cmd = ["cc", "-O1", "-shared", "-fPIC", *strict_options(), "-I", ROOT / "core/include",
           "-I", ROOT / "devices/tsop382", HERE / "wire_layers.c", ROOT / "core/src/onewire.c",
           ROOT / "devices/tsop382/nec.c", "-o", lib]
    result = subprocess.run(cmd, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    layers = ctypes.CDLL(str(lib))
    layers.layers_onewire_count.restype = ctypes.c_uint32
    layers.layers_nec_get.restype = ctypes.c_uint32
    layers.layers_nec_take.argtypes = (ctypes.c_int, ctypes.c_uint32)
    return dump, layers


def programs(built, hz):
    out = subprocess.run([built[0], str(hz)], capture_output=True, text=True, check=True).stdout
    found = {}
    for line in out.splitlines():
        name, *rest = line.split()
        if name == "cycles":
            found[name] = tuple(int(v) for v in rest)
        else:
            found[name] = [int(v, 16) for v in rest[1:]]
    return found


def tool(built, hz, mode, *values):
    out = subprocess.run([built[0], str(hz), mode, *map(str, values)], capture_output=True, text=True, check=True)
    return [int(v, 16) if mode != "enabled" else int(v) for v in out.stdout.split()]


def feeder(sim, sm, words, every=200 * NS):
    # The CPU, or a DMA channel, keeping a TX FIFO topped up.
    queue = list(words)

    def top_up(now):
        while queue and not sm.tx_full():
            sm.put(queue.pop(0))
        if queue:
            sim.at(now + every, top_up)

    sim.at(sim.t, top_up)
    return queue


def drain(sm):
    sm.rx_depth = 1 << 30


# --- WS2812: pio/ws2812/ws2812.pio, ws2812.c:126 (800 kHz, 10 cycles a bit)

WS2812 = """
.side_set 1
.define public T1 3
.define public T2 3
.define public T3 4
.wrap_target
bitloop:
    out x, 1       side 0 [T3 - 1]
    jmp !x do_zero side 1 [T1 - 1]
do_one:
    jmp  bitloop   side 1 [T2 - 1]
do_zero:
    nop            side 0 [T2 - 1]
.wrap
"""

WS2812_PARALLEL = """
.define public T1 3
.define public T2 3
.define public T3 4
.wrap_target
    out x, 32
    mov pins, !null [T1-1]
    mov pins, x     [T2-1]
    mov pins, null  [T3-2]
.wrap
"""


def pulse_bits_harness(sim, built, hz, pin):
    progs = programs(built, hz)
    sm = StateMachine(sim, f"harness{pin}", progs["pulse_bits"], hz=hz, in_base=pin, jmp_pin=pin, in_right=False,
                      autopush=True, push_at=8)
    sm.y = progs["cycles"][0] // 2
    drain(sm)
    return sm


@pytest.mark.parametrize("hz", HARNESS_CLOCKS)
def test_pulse_bits_reads_the_ws2812_programs_pixels(built, hz):
    words, _, wrap_target, wrap, *_ = assemble(WS2812)
    sim = Sim()
    dut = StateMachine(sim, "dut", words, hz=8e6, sys_hz=125e6, wrap=wrap, wrap_target=wrap_target, sideset_base=0,
                       sideset_bits=1, out_right=False, autopull=True, pull_at=24, join="tx")
    dut.init_pins({0: 0}, {0: 1})
    dut.next_t = 37 * NS
    harness = pulse_bits_harness(sim, built, hz, 0)
    pixels = [0x00FF00, 0xA5C3E1, 0x000000, 0xFFFFFF, 0x123456]
    feeder(sim, dut, [p << 8 for p in pixels])
    sim.run_for(len(pixels) * 24 * 1.25 * US + 60 * US)
    want = [b for p in pixels for b in ((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF)]
    assert list(harness.rx) == want + [END]


@pytest.mark.parametrize("hz", HARNESS_CLOCKS)
def test_pulse_bits_reads_both_strips_of_the_parallel_program(built, hz):
    words, _, wrap_target, wrap, *_ = assemble(WS2812_PARALLEL)
    sim = Sim()
    dut = StateMachine(sim, "dut", words, hz=8e6, sys_hz=125e6, wrap=wrap, wrap_target=wrap_target, out_base=0,
                       out_count=2, out_right=True, autopull=True, pull_at=32, join="tx")
    dut.init_pins({0: 0, 1: 0}, {0: 1, 1: 1})
    strips = [bytes(range(10, 40, 3)), bytes(range(200, 250, 5))]
    planes = []
    for v in range(len(strips[0])):
        for bit in range(7, -1, -1):
            planes.append(sum(((strips[s][v] >> bit) & 1) << s for s in range(2)))
    harnesses = [pulse_bits_harness(sim, built, hz, pin) for pin in (0, 1)]
    feeder(sim, dut, planes)
    sim.run_for(len(planes) * 1.25 * US + 60 * US)
    for s in range(2):
        assert list(harnesses[s].rx) == list(strips[s]) + [END]


def pulses(widths_ns, period_ns=1250):
    # A push-pull line: each width a high pulse at the start of its period.
    edges, t = [], 10 * US
    for w in widths_ns:
        edges += [(t, 1), (t + w * NS, 0)]
        t += period_ns * NS
    return edges, t


@pytest.mark.parametrize("hz", HARNESS_CLOCKS)
def test_pulse_bits_follows_the_datasheets_windows(built, hz):
    # WS2812B: T0H 0.4 us and T1H 0.8 us, +-150 ns; RES above 50 us.
    sim = Sim()
    harness = pulse_bits_harness(sim, built, hz, 0)
    good = [260, 540, 660, 940, 400, 800, 400, 800]
    edges, t = pulses(good)
    sim.waveform("dut", 0, edges)
    sim.run_until(t + 45 * US)
    assert list(harness.rx) == [0b00110101]
    sim.run_until(t + 60 * US)
    assert list(harness.rx) == [0b00110101, END]
    for width in (180, 1030):
        harness.rx.clear()
        edges, t = pulses([400] * 3 + [width] + [400] * 4, 1300)
        sim.waveform("dut", 0, [(e + sim.t, v) for e, v in edges])
        sim.run_until(sim.t + t + 60 * US)
        assert list(harness.rx) == [BAD, END], width


# --- APA102: pio/apa102/apa102.pio (apa102_mini), apa102.c:18, 54 (5 MHz)

APA102 = """
.side_set 1
    out pins, 1   side 0
    nop           side 1
"""


@pytest.mark.parametrize("hz", HARNESS_CLOCKS)
def test_clocked_in_takes_every_bit_of_the_apa102_program(built, hz):
    words, *_ = assemble(APA102)
    sim = Sim()
    dut = StateMachine(sim, "dut", words, hz=10e6, sys_hz=125e6, out_base=1, out_count=1, sideset_base=0,
                       sideset_bits=1, out_right=False, autopull=True, pull_at=32, join="tx")
    dut.init_pins({0: 0, 1: 0}, {0: 1, 1: 1})
    harness = StateMachine(sim, "harness", programs(built, hz)["clocked_in"], hz=hz, in_base=1, in_right=False,
                           autopush=True, push_at=32)
    drain(harness)
    sent = [0, 0xF0102030, 0xF0FFEE00, 0xE1000001, 0xFFFFFFFF]
    feeder(sim, dut, sent)
    sim.run_for(len(sent) * 32 * 200 * NS + 2 * US)
    assert list(harness.rx) == sent


# --- HUB75: pio/hub75/hub75.pio, hub75.c:37-78 (data at the system clock)

HUB75_ROW = """
.side_set 2
.wrap_target
    out pins, 5 [7]    side 0x2
    out x, 27   [7]    side 0x3
pulse_loop:
    jmp x-- pulse_loop side 0x0
.wrap
"""

HUB75_DATA = """
.side_set 1
public entry_point:
.wrap_target
public shift0:
    pull             side 0
    in osr, 1        side 0
    out null, 8      side 0
    in osr, 1        side 0
    out null, 8      side 0
    in osr, 1        side 0
    out null, 32     side 0
public shift1:
    pull             side 0
    in osr, 1        side 1
    out null, 8      side 1
    in osr, 1        side 1
    out null, 8      side 1
    in osr, 1        side 1
    out null, 32     side 1
    in null, 26      side 1
    mov pins, ::isr  side 1
.wrap
"""

CLK, LAT, OEN = 11, 12, 13


def hub75_dut(sim, dut_hz):
    data_words, labels, wrap_target, wrap, *_ = assemble(HUB75_DATA)
    row_words, _, row_target, row_wrap, *_ = assemble(HUB75_ROW)
    data = StateMachine(sim, "dut_data", data_words, hz=dut_hz, wrap=wrap, wrap_target=wrap_target, out_base=0,
                        out_count=6, sideset_base=CLK, sideset_bits=1, out_right=True, autopull=True, pull_at=24,
                        in_right=False, join="tx")
    row = StateMachine(sim, "dut_row", row_words, hz=dut_hz, wrap=row_wrap, wrap_target=row_target, out_base=6,
                       out_count=5, sideset_base=LAT, sideset_bits=2, out_right=True, autopull=True, pull_at=32)
    for sm, pins in ((data, [*range(6), CLK]), (row, [*range(6, 11), LAT, OEN])):
        sm.init_pins({p: 0 for p in pins}, {p: 1 for p in pins})
    return data, row, labels


def hub75_cpu(sim, data, row, labels, rows, pixels):
    # hub75.c:52-75 for the given rows, its put_blocking and
    # hub75_wait_tx_stall() run against the emulated state machines.
    def wait(cond, step=100 * NS):
        while not cond():
            sim.run_for(step)

    for rowsel in rows:
        for bit in range(8):
            word = 0x80A0 if bit == 0 else 0x6060 | bit
            data.mem[labels["shift0"]] = data.mem[labels["shift1"]] = word
            for value in [v for pair in pixels(rowsel) for v in pair] + [0, 0]:
                wait(lambda: not data.tx_full())
                data.put(value)
            for sm in (data, row):
                sm.tx_stalled = False
                wait(lambda: sm.tx_stalled and not sm.tx)
            row.put(rowsel | (100 * (1 << bit)) << 5)
            # The loop around to the next plane's first put, set_shift()'s
            # two stores among it: some 20 cycles of the M0+.
            sim.run_for(160 * NS)


def gc_pixels(rowsel):
    rng = random.Random(rowsel)
    return [(rng.getrandbits(24), rng.getrandbits(24)) for _ in range(128)]


@pytest.mark.parametrize("dut_hz,hz", [(125e6, 150_000_000), (150e6, 125_000_000), (125e6, 125_000_000)])
def test_latched_in_captures_a_hub75_latch_and_its_enable_time(built, dut_hz, hz):
    sim = Sim()
    data, row, labels = hub75_dut(sim, dut_hz)
    progs = programs(built, hz)
    cap = StateMachine(sim, "harness_data", progs["latched_data"], hz=hz, in_base=CLK, jmp_pin=LAT, in_right=True,
                       autopush=True, push_at=32, out_right=True)
    enable = StateMachine(sim, "harness_enable", progs["latched_enable"], hz=hz, in_base=OEN, jmp_pin=LAT,
                          in_right=True, out_right=True)
    drain(cap)
    hub75_cpu(sim, data, row, labels, [5], gc_pixels)
    sim.run_for(20 * US)
    assert enable.rx, "no capture completed"
    samples = [((w << CLK) | (w >> (32 - CLK))) & 0xFFFFFFFF for w in cap.rx]
    clocks, latch = samples[:-1], samples[-1]
    assert all(s >> CLK & 1 for s in clocks) and not latch >> CLK & 1
    assert len(clocks) == 129
    # The latch after plane 0's took plane 1, lit for 100 << 1 cycles.
    want = []
    for top, bottom in gc_pixels(5):
        want.append(sum(((v >> (8 * ch + 1)) & 1) << (3 * half + ch) for half, v in ((0, top), (1, bottom))
                        for ch in range(3)))
    assert [s & 0x3F for s in clocks[1:]] == want
    assert (latch >> 6) & 0x1F == 5
    (enabled_ns,) = tool(built, hz, "enabled", enable.rx[0])
    assert abs(enabled_ns - 200 / dut_hz * 1e9) <= 5 * 1e9 / hz + 1


# --- 1-Wire: pio/onewire/onewire_library/onewire_library.pio and .c

ONEWIRE_MASTER = """
.side_set 1 pindirs
PUBLIC reset_bus:
        set x, 28       side 1  [15]
loop_a: jmp x-- loop_a  side 1  [15]
        set x, 8        side 0  [6]
loop_b: jmp x-- loop_b  side 0  [6]
        mov isr, pins   side 0
        push            side 0
        set x, 24       side 0  [7]
loop_c: jmp x-- loop_c  side 0  [15]
.wrap_target
PUBLIC fetch_bit:
        out x, 1        side 0
        jmp !x  send_0  side 1  [5]
send_1:
        set x, 2        side 0  [8]
        in pins, 1      side 0  [4]
loop_e: jmp x-- loop_e  side 0  [15]
        jmp fetch_bit   side 0
send_0:
        set x, 2        side 1  [5]
loop_d: jmp x-- loop_d  side 1  [15]
        in null, 1      side 0  [8]
.wrap
"""


class OneWireMaster:
    # onewire_library.c's driver, its FIFO traffic against the emulated
    # state machine.
    def __init__(self, sim, sm, labels):
        self.sim, self.sm, self.labels = sim, sm, labels

    def wait_rx(self):
        assert self.sim.run_for(50 * MS, until=lambda: bool(self.sm.rx)), "the master's slot never ended"
        return self.sm.get()

    def init(self, bits):
        sm = self.sm
        sm.push_at = sm.pull_at = bits
        sm.rx.clear()
        sm.tx.clear()
        sm.restart(sm.offset + self.labels["fetch_bit"])

    def reset(self):
        self.sm.exec(0x0000 | self.labels["reset_bus"])
        return (self.wait_rx() & 1) == 0

    def send(self, data):
        self.sm.put(data)
        self.wait_rx()

    def read(self):
        self.sm.put(0xFF)
        return (self.wait_rx() >> 24) & 0xFF

    def romsearch(self, command=0xF0):
        # onewire_library.c:170-230, for up to ten devices.
        romcodes, next_branch, found, finished = [], -1, 0, False
        self.init(1)
        while not finished and found < 10:
            finished, branch, romcode = True, next_branch, 0
            if not self.reset():
                found = 0
                break
            for i in range(8):
                self.send(command >> i)
            for index in range(64):
                a, b = self.read(), self.read()
                if a == 0 and b == 0:
                    if index == branch:
                        self.send(1)
                        romcode |= 1 << index
                    elif index > branch or not romcode >> index & 1:
                        self.send(0)
                        finished, next_branch = False, index
                        romcode &= ~(1 << index)
                    else:
                        self.send(1)
                elif a and b:
                    found, finished = -2, True
                    break
                else:
                    self.send(0 if a == 0 else 1)
                    romcode |= (1 << index) if a else 0
            romcodes.append(romcode)
            found += 1
        self.init(8)
        return found, romcodes


def onewire_bench(built, latency_us):
    sim = Sim()
    sim.bus.pull(0, "up")
    words, labels, wrap_target, wrap, *_ = assemble(ONEWIRE_MASTER)
    master = StateMachine(sim, "dut", words, hz=1e6, sys_hz=125e6, wrap=wrap, wrap_target=wrap_target, in_base=0,
                          sideset_base=0, sideset_bits=1, sideset_pindirs=True, in_right=True, autopush=True,
                          push_at=8, out_right=True, autopull=True, pull_at=8)
    master.init_pins({0: 0}, {0: 0})
    master.pc = labels["fetch_bit"]
    part = StateMachine(sim, "harness", programs(built, 150_000_000)["onewire"], hz=1e6, sys_hz=150e6, in_base=0,
                        jmp_pin=0, sideset_base=0, sideset_bits=1, sideset_pindirs=True, in_right=False,
                        out_right=True)
    part.init_pins({0: 0}, {0: 0})
    part.next_t = 370 * NS
    lib = built[1]
    rom = lights.ds18b20_rom(0xC0FFEE).to_bytes(8, "little")
    memory = bytes((0x56, 0x01, 0x7D, 0xC9, 0x7F, 0xFF, 0xFF, 0x10, 0x00))
    lib.layers_onewire_init(rom, memory)

    # The slot handler: each word the part's program pushes reaches the
    # 1-Wire layer `latency_us` later, and its answer goes into the TX FIFO.
    def handler(now):
        while part.rx:
            word = part.get()

            def answer(t, word=word):
                if word == END:
                    lib.layers_onewire_reset()
                else:
                    part.put(lib.layers_onewire_slot(word & 1))
            sim.at(now + latency_us * US, answer)
        sim.at(now + 500 * NS, handler)

    sim.at(0, handler)
    return sim, OneWireMaster(sim, master, labels), lib, rom


@pytest.mark.parametrize("latency_us", (1, 20))
def test_onewire_answers_the_programs_search_conversion_and_read(built, latency_us):
    # onewire.c:36-66: search, then Skip ROM and Convert T, read until the
    # status turns 1, Match ROM and Read Scratchpad.
    sim, master, lib, rom = onewire_bench(built, latency_us)
    found, codes = master.romsearch()
    assert (found, codes) == (1, [int.from_bytes(rom, "little")])
    # onewire.c:38-42 prints what it found before its next reset; a reset
    # within the last slot would cut it short for any DS18B20.
    sim.run_for(1 * MS)
    assert master.reset()
    master.send(0xCC)
    master.send(0x44)
    assert [master.read() for _ in range(3)] == [0, 0, 0]
    lib.layers_onewire_status(1)
    reads = [master.read() for _ in range(2)]
    assert reads[0] in (0xFE, 0xFF) and reads[1] == 0xFF
    assert master.reset()
    master.send(0x55)
    for byte in rom:
        master.send(byte)
    master.send(0xBE)
    temp = master.read() | master.read() << 8
    assert temp / 16.0 == 21.375
    assert [lib.layers_onewire_count(i) for i in (2, 3, 4)] == [1, 1, 2]


def test_onewire_presence_follows_figure_13(built):
    sim, master, lib, _ = onewire_bench(built, 1)
    assert master.reset()
    sim.run_for(300 * US)
    line = sim.bus.lines[0].edges()
    # The master's 480 us low, then the part's presence pulse: low 30 us
    # after the release, for 120 us (15-60 and 60-240 us).
    lows = [(t0, t1 - t0) for (t0, v0), (t1, _) in zip(line, line[1:]) if v0 == 0]
    master_low, presence = lows[-2], lows[-1]
    assert abs(master_low[1] - 480 * US) <= 2 * US
    gap = presence[0] - (master_low[0] + master_low[1])
    assert 15 * US <= gap <= 60 * US and 60 * US <= presence[1] <= 240 * US


# --- DHT22 against gpio/dht_sensor/dht.c's bit-banged read

def dht_bench(built, pulses_ns, harness_hz=10_000_000):
    sim = Sim()
    sim.bus.pull(0, "up")
    words = programs(built, harness_hz)["responder"]
    part = StateMachine(sim, "harness", words, hz=harness_hz, in_base=0, jmp_pin=0, sideset_base=0,
                        sideset_bits=1, sideset_pindirs=True, out_right=True, join="tx")
    part.init_pins({0: 0}, {0: 0})
    part.isr = 1_000_000 * harness_hz // 1_000_000_000 // 2
    feeder(sim, part, tool(built, harness_hz, "respond", *(f"{p:x}" for p in pulses_ns)) + [0], every=1 * US)
    # dht.c:49-52: GPIO out low for 20 ms, then input.
    sim.drive(1 * MS, "dut", 0, True, 0)
    sim.drive(21 * MS, "dut", 0, False)
    sim.run_until(28 * MS)
    return sim.bus.lines[0]


def dht_pulses(frame):
    # The DHT22 datasheet waveform devices/dht22 builds (DHT22_* in dht22.h).
    out = [30000, 0x80000000 | 80000, 80000]
    for i in range(40):
        out += [0x80000000 | 50000, 70000 if frame >> (39 - i) & 1 else 27000]
    return out + [0x80000000 | 50000]


def test_responder_plays_the_dht22_waveform_after_the_programs_start(built):
    frame = 0x01C800EA_B3
    line = dht_bench(built, dht_pulses(frame))
    edges = line.edges()
    release = next(t for t, v in edges if v == 1 and t > 20 * MS)
    after = [(t - release, v) for t, v in edges if t >= release]
    widths = [(b[0] - a[0], a[1]) for a, b in zip(after, after[1:])]
    want = [(p & 0x7FFFFFFF, 0 if p >> 31 else 1) for p in dht_pulses(frame)]
    assert len(widths) == len(want)
    for (got, level), (ns, want_level) in zip(widths, want):
        assert level == want_level and abs(got - ns * NS) <= 1.5 * US, (got, ns)
    assert after[-1][1] == 1
    # dht.c reads 45.6 %RH and 23.4 degC from it when a pass of its loop
    # takes 2 us, and every bit as 1 when a pass takes 1 us.
    levels = [line.at(release + k * US) for k in range(6000)]
    assert dht_read(levels, 2) == "Humidity = 45.6%, Temperature = 23.4C (74.1F)\n"
    assert dht_read(levels, 1) == "Bad data\n"


# --- quadrature encoders: pio/quadrature_encoder/quadrature_encoder.pio and
# pio/quadrature_encoder_substep/quadrature_encoder_substep.pio

QUADRATURE = """
.origin 0
    JMP update
    JMP decrement
    JMP increment
    JMP update
    JMP increment
    JMP update
    JMP update
    JMP decrement
    JMP decrement
    JMP update
    JMP update
    JMP increment
    JMP update
    JMP increment
decrement:
    JMP Y--, update
.wrap_target
update:
    MOV ISR, Y
    PUSH noblock
sample_pins:
    OUT ISR, 2
    IN PINS, 2
    MOV OSR, ISR
    MOV PC, ISR
increment:
    MOV Y, ~Y
    JMP Y--, increment_cont
increment_cont:
    MOV Y, ~Y
.wrap
"""

SUBSTEP = """
.origin 0
    IN X, 32
    IN Y, 32
update_state:
    OUT ISR, 2
    IN PINS, 2
    MOV OSR, ~ISR
    MOV PC, OSR
decrement:
    JMP Y--, decrement_cont
decrement_cont:
    SET X, 1
    MOV X, ::X
check_fifo:
.wrap_target
    JMP X--, check_fifo_cont
check_fifo_cont:
    MOV PC, ~STATUS
increment:
    MOV Y, ~Y
    JMP Y--, increment_cont
increment_cont:
    MOV Y, ~Y
    SET X, 0
    .wrap
invalid:
    JMP update_state
    JMP invalid
    JMP increment       [0]
    JMP decrement       [1]
    JMP check_fifo      [4]
    JMP decrement       [1]
    JMP invalid
    JMP check_fifo      [4]
    JMP increment       [0]
    JMP increment       [0]
    JMP check_fifo      [4]
    JMP invalid
    JMP decrement       [1]
    JMP check_fifo      [4]
    JMP decrement       [1]
    JMP increment       [0]
    JMP update_state    [1]
"""

GRAY = (0, 2, 3, 1)


def encoder_words(moves):
    # devices/quadrature_encoder's states: position p is GRAY[p % 4], A in
    # bit 0, B in bit 1, sixteen to a word.
    states, p = [], 0
    for n in moves:
        for _ in range(abs(n)):
            p += 1 if n > 0 else -1
            states.append(GRAY[p % 4])
    states += [states[-1]] * (-len(states) % 16)
    return [sum(s << (2 * i) for i, s in enumerate(states[k:k + 16])) for k in range(0, len(states), 16)], p


def steps_out(sim, built, hz, period_ns):
    sm = StateMachine(sim, "harness", programs(built, hz)["steps_out"], hz=hz, out_base=0, out_count=2,
                      out_right=True, autopull=True, pull_at=32, join="tx")
    sm.init_pins({0: 0, 1: 0}, {0: 1, 1: 1})
    sm.y = round(period_ns * hz / 1e9) - 3
    return sm


def test_steps_out_turns_the_quadrature_programs_count(built):
    words, _, wrap_target, wrap, *_ = assemble(QUADRATURE)
    assert len(words) == 24
    sim = Sim()
    dut = StateMachine(sim, "dut", words, hz=125e6, wrap=wrap, wrap_target=wrap_target, in_base=0, jmp_pin=0,
                       in_right=False)
    harness = steps_out(sim, built, 150_000_000, 400)
    out, position = encoder_words([300, -100])
    feeder(sim, harness, out)
    sim.run_for(len(out) * 16 * 400 * NS + 5 * US)
    # quadrature_encoder_get_count(): drain, then one fresh count.
    dut.rx.clear()
    sim.run_for(1 * US, until=lambda: bool(dut.rx))
    assert (dut.get() + 2**31) % 2**32 - 2**31 == position == 200


def test_steps_out_turns_the_substep_programs_count_and_direction(built):
    words, labels, wrap_target, wrap, *_ = assemble(SUBSTEP)
    assert len(words) == 32 and labels["invalid"] == 15
    sim = Sim()
    dut = StateMachine(sim, "dut", words, hz=125e6, wrap=wrap, wrap_target=wrap_target, in_base=0, in_right=False,
                       autopush=True, push_at=32, out_right=True, status_sel=1, status_n=2)
    # quadrature_encoder_substep_program_init(): the pins read 00.
    dut.y, dut.osr, dut.osr_count = 31, 31, 0
    dut.y = 0
    harness = steps_out(sim, built, 150_000_000, 400)
    out, position = encoder_words([300, -100])
    feeder(sim, harness, out)
    sim.run_for(len(out) * 16 * 400 * NS + 5 * US)
    dut.rx.clear()
    sim.run_for(2 * US, until=lambda: len(dut.rx) >= 2)
    cycles, step = dut.get(), dut.get()
    assert (step, cycles >= 2**31) == (200, False) and position == 200


# --- NEC IR loopback: pio/ir_nec/nec_transmit_library/*.pio, nec_receive_library/nec_receive.pio

NEC_BURST = """
.define NUM_CYCLES 21
.define BURST_IRQ 7
.define public TICKS_PER_LOOP 4
.wrap_target
    set X, (NUM_CYCLES - 1)
    wait 1 irq BURST_IRQ
cycle_loop:
    set pins, 1
    set pins, 0 [1]
    jmp X--, cycle_loop
.wrap
"""

NEC_CONTROL = """
.define BURST_IRQ 7
.define NUM_INITIAL_BURSTS 16
.wrap_target
    pull
    set X, (NUM_INITIAL_BURSTS - 1)
long_burst:
    irq BURST_IRQ
    jmp X-- long_burst
    nop [15]
    irq BURST_IRQ [1]
data_bit:
    out X, 1
    jmp !X burst
    nop [3]
burst:
    irq BURST_IRQ
jmp !OSRE data_bit
.wrap
"""

NEC_RECEIVE = """
.define BURST_LOOP_COUNTER 30
.define BIT_SAMPLE_DELAY 15
.wrap_target
next_burst:
    set X, BURST_LOOP_COUNTER
    wait 0 pin 0
burst_loop:
    jmp pin data_bit
    jmp X-- burst_loop
    mov ISR, NULL
    wait 1 pin 0
    jmp next_burst
data_bit:
    nop [ BIT_SAMPLE_DELAY - 1 ]
    in PINS, 1
.wrap
"""


def test_envelope_carries_the_loopback_programs_frame_and_times_it(built):
    sim = Sim()
    tx, rx = 0, 1
    burst_words, _, bt, bw, *_ = assemble(NEC_BURST)
    control_words, _, ct, cw, *_ = assemble(NEC_CONTROL)
    receive_words, _, rt, rw, *_ = assemble(NEC_RECEIVE)
    burst = StateMachine(sim, "burst", burst_words, hz=38.222e3 * 4, sys_hz=125e6, wrap=bw, wrap_target=bt,
                         set_base=tx, set_count=1)
    burst.init_pins({tx: 0}, {tx: 1})
    control = StateMachine(sim, "control", control_words, hz=2 / 562.5e-6, sys_hz=125e6, wrap=cw, wrap_target=ct,
                           out_right=True, autopull=False, pull_at=32, join="tx")
    receive = StateMachine(sim, "receive", receive_words, hz=10 / 562.5e-6, sys_hz=125e6, wrap=rw, wrap_target=rt,
                           in_base=rx, jmp_pin=rx, in_right=True, autopush=True, push_at=32, join="rx")
    envelope = StateMachine(sim, "harness", programs(built, 150_000_000)["envelope"], hz=1e6, sys_hz=150e6,
                            in_base=tx, jmp_pin=tx, sideset_base=rx, sideset_bits=1, join="rx")
    envelope.init_pins({rx: 1}, {rx: 1})
    envelope.osr = round(52000 * 1e6 / 1e9) // 3
    drain(envelope)
    frame = 0x00 | 0xFF << 8 | 0x05 << 16 | 0xFA << 24
    control.put(frame)
    sim.run_until(90 * MS)
    assert list(receive.rx) == [frame]
    lib = built[1]
    lib.layers_nec_init()
    for entry in tool(built, 1_000_000, "envelope", *(f"{w:x}" for w in envelope.rx)):
        lib.layers_nec_take(entry >> 31, entry & 0x7FFFFFFF)
    assert [lib.layers_nec_get(i) for i in range(4)] == [1, 0, 0, frame]
