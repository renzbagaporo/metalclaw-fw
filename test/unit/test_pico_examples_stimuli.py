# SPDX-License-Identifier: Apache-2.0 OR MIT
# The outside world the rig gives pico-examples programs that need more than
# a classic part: stimuli, jumpers, SPI peers, console input, and the pin
# checks that judge them. Each program's output is transcribed from its
# source (pico-examples 0d62f75) for what the rig sets up, and must satisfy
# its Spec; the pin checks run on samples made the way the program drives
# its pins.

import itertools
import types

import pytest

from test.pico_examples import pins
from test.pico_examples.cases import Typist, pin_actions
from test.pico_examples.part_targets import PART_SPECS
from test.pico_examples.parts import DOWN, PARTS, UP, Attach, Check, PinError
from test.pico_examples.pin_specs import BOOTSEL_SPECS, PIN_SPECS, PinSpec, Press
from test.pico_examples.targets import SPECS, Spec
from test.rig.platforms.hardware import dap
from test.rig.platforms.hardware.platform import FIXTURE, METALCLAW_PICO_RP2_FIXTURE

FIXTURES = (FIXTURE, METALCLAW_PICO_RP2_FIXTURE)
CARRIER_ONLY = {"hello_gpio_irq", "pio_uart_rx", "pio_uart_rx_intr", "pio_manchester_encoding",
                "pio_differential_manchester", "pwm_measure_duty_cycle", "pio_clocked_input",
                "pio_ws2812", "pio_ws2812_parallel", "pio_apa102", "pio_hub75", "pio_i2c_bus_scan"}


def spec(target):
    return SPECS.get(target) or PART_SPECS[target]


def judged(target, text, line_pullups=False, analog_pins=()):
    s = spec(target).for_bench(line_pullups, analog_pins)
    assert s.match(text) == ([], []), (target, s.match(text), text[-500:])


# ---- attaching ---------------------------------------------------------


def test_the_carrier_only_targets_are_exactly_those_on_gp2_or_gp3():
    for target, parts in PARTS.items():
        uses = {p for part in parts for p in part.dut_pins()}
        if uses & {2, 3}:
            assert target in CARRIER_ONLY, target
    for target in CARRIER_ONLY:
        with pytest.raises(PinError):
            for part in PARTS[target]:
                part.code(FIXTURE)
        for part in PARTS[target]:
            assert "metalclaw.attach" in part.code(METALCLAW_PICO_RP2_FIXTURE)


def test_a_jumper_takes_both_pins_from_one_window():
    (tx,) = PARTS["pio_uart_dma"]
    assert "source=(4,), sink=(5,)" in tx.code(FIXTURE)
    assert "source=(11,), sink=(10,)" in tx.code(METALCLAW_PICO_RP2_FIXTURE)
    sda, scl = PARTS["slave_mem_i2c"]
    assert sda.model == scl.model == "jumper_open_drain"
    assert "a=(4,), b=(6,)" in sda.code(FIXTURE) and "a=(5,), b=(7,)" in scl.code(FIXTURE)
    assert "a=(11,), b=(14,)" in sda.code(METALCLAW_PICO_RP2_FIXTURE)


def test_one_window_refuses_pins_split_across_windows():
    # DUT GP20 has only GPIO40/45 on the carrier and GP5 only GPIO10/13.
    with pytest.raises(PinError):
        Attach("jumper", "j", {"source": (20,), "sink": (5,)}, one_window=True).code(METALCLAW_PICO_RP2_FIXTURE)


def test_the_spi_peers_take_the_carrier_window_branches():
    (target,) = PARTS["spi_master"]
    (controller,) = PARTS["spi_slave"]
    assert "sck=(18,), mosi=(19,), miso=(16,), cs=(17,)" in target.code(FIXTURE)
    assert "sck=(38,), mosi=(32,), miso=(35,), cs=(34,)" in target.code(METALCLAW_PICO_RP2_FIXTURE)
    assert "sck=(38,), mosi=(35,), miso=(32,), cs=(34,)" in controller.code(METALCLAW_PICO_RP2_FIXTURE)


def test_an_analog_level_is_a_pwm_on_the_carrier_and_a_rail_on_fixture_1():
    x, y = PARTS["joystick_display"]
    assert "x.period = 1000\nx.high = 250\n" in x.code(METALCLAW_PICO_RP2_FIXTURE)
    assert "x.period = 0\nx.high = 0\nx.level = False\n" in x.code(FIXTURE)
    assert "y.level = True\n" in y.code(FIXTURE)


def test_the_rtc_clock_is_only_for_the_pico_build():
    (clock,) = PARTS["low_power_dormant_timer"]
    assert clock.boards == ("pico",)
    assert "rtc_clock.period = 21333\nrtc_clock.high = 10667\n" in clock.code(FIXTURE)
    # 125 MHz / 2667 cycles, what the PWM makes of 21333 ns: within 0.02 %
    # of RTC_CLOCK_FREQ_HZ 46875.
    assert abs(125e6 / round(21333 * 0.125) - 46875) / 46875 < 2e-4


# A program that prints nothing is judged by its pins or its parts' checks.
def test_every_rig_target_has_criteria_and_a_source():
    for target, parts in PARTS.items():
        console = SPECS.get(target) or PART_SPECS.get(target)
        assert (console and console.source) or target in PIN_SPECS or any(p.checks for p in parts), target
    for target in ("picoboard_blinky", "picoboard_button", "hello_7segment", "hello_pio"):
        assert PIN_SPECS[target].source
    assert BOOTSEL_SPECS["hello_double_tap"].source


# ---- console input -----------------------------------------------------


class FakePort:
    def __init__(self):
        self.written = []

    def write(self, data):
        self.written.append(data)

    def flush(self):
        pass


class FakeHarness:
    def __init__(self):
        self.ran = []

    def run(self, code):
        self.ran.append(code)
        return ""


def test_inputs_go_out_in_order_once_each_after_their_pattern():
    port, harness = FakePort(), FakeHarness()
    t = Typist(port, ((r"ready", b"a"), (r"ready", b"b"), (r"go", "part.pages = 1")), harness)
    t("rea")
    assert port.written == []
    t("ready")
    assert port.written == [b"a"]
    t("ready ready")
    assert port.written == [b"a", b"b"] and harness.ran == []
    t("ready ready go")
    t("ready ready go go")
    assert harness.ran == ["part.pages = 1"]


def run_console(target, respond):
    # The program's output so far, the bytes the case types, and the
    # program's answer to each: until nothing more is typed.
    s = spec(target)
    port = FakePort()
    typist = Typist(port, s.input, FakeHarness())
    text = respond(None)
    for _ in range(10):
        typist(text)
        if not port.written:
            break
        typed, port.written = b"".join(port.written), []
        text += respond(typed)
    return text


def test_adc_console_answers_what_the_case_types():
    help_text = "\r\nCommands:\r\nc0, ...\t: Select ADC channel n\r\ns\t: Sample once\r\nS\t: Sample many\r\nw\t: Wiggle pins\r\n"

    def program(typed):
        if typed is None:
            return "\r\n===========================\r\nRP2040 ADC and Test Console\r\n" \
                   "===========================\r\n" + help_text
        out = ""
        chars = iter(typed.decode())
        for c in chars:
            out += c
            if c == "c":
                n = next(chars)
                out += f"{n}\r\nSwitched to channel {n}\r\n"
            elif c == "s":
                raw = 0x36B
                out += f"\r\n0x{raw:03x} -> {raw * 3.3 / 4096:f} V\r\n"
            else:
                out += f"\r\nUnrecognised command: {c}\r\n" + help_text
        return out

    text = run_console("adc_console", program)
    assert "w" not in "".join(i[1].decode() for i in SPECS["adc_console"].input)
    judged("adc_console", text)


def test_uart_advanced_echoes_each_character_plus_one():
    text = run_console("uart_advanced", lambda typed: "\nHello, uart interrupts\n" if typed is None
                       else bytes(b + 1 for b in typed).decode())
    judged("uart_advanced", text)
    assert text.endswith("IBM")


def test_spi_slave_prints_the_pages_the_controller_is_told_to_clock():
    def rows(page):
        return "".join(" ".join(f"{page[16 * r + c]:02x}" for c in range(16)) + "\r\n" for r in range(16))

    def program(typed):
        return ("SPI slave example\r\nSPI slave says: When reading from MOSI, the following buffer will be "
                "written to MISO:\r\n" + rows(DOWN))

    s = spec("spi_slave")
    harness = FakeHarness()
    typist = Typist(FakePort(), s.input, harness)
    text = program(None)
    typist(text)
    assert harness.ran == ["peer.pages = 3"]
    text += "".join(f"SPI slave says: read page {p} from the MOSI line:\r\n" + rows(UP) for p in range(3))
    judged("spi_slave", text)


def test_spi_master_reads_the_slaves_page_back():
    def rows(page):
        return "".join(" ".join(f"{page[16 * r + c]:02x}" for c in range(16)) + "\r\n" for r in range(16))

    text = ("SPI master example\r\nSPI master says: The following buffer will be written to MOSI endlessly:\r\n"
            + rows(UP) + "".join(f"SPI master says: read page {p} from the MISO line:\r\n" + rows(DOWN)
                                 for p in range(3)))
    judged("spi_master", text)
    bad = text.replace("fe fd fc", "fe fd fd")
    assert spec("spi_master").match(bad)[0]


def test_the_peer_checks_want_the_partner_programs_pages():
    (target,) = PARTS["spi_master"]
    (controller,) = PARTS["spi_slave"]
    assert target.checks[0].test(bytes(range(256))) and not target.checks[0].test(bytes(256))
    assert controller.checks[0].test(bytes(255 - i for i in range(256)))
    assert controller.checks[0].setup == "part.pages = 1"
    assert isinstance(controller.checks[0], Check)


# ---- stimuli -----------------------------------------------------------


def low_power_run(target, asleep, start_high, runs=5, boot=False):
    # The program's lines as GP15's wave drives it: the restart finds GP15
    # high or low; every wake is a rising edge, with 900 ms high after it.
    out = []
    if boot:
        out += ["Wake up, test run: 1234\n", f"State of gpio 15 is {int(start_high)}\n"]
        if start_high:
            out.append("Awake until gpio 15 goes low\n")
        out.append(asleep + "\n")
        for n in range(runs):
            out += [f"Wake up, test run: {1235 + n}\n", "State of gpio 15 is 1\n",
                    "Awake until gpio 15 goes low\n", asleep + "\n"]
        return "".join(out)
    out.append(f"State of gpio 15 is {int(start_high)}\n")
    for n in range(1, runs + 1):
        out.append(f"Wake up, test run: {n}\n")
        if n > 1 or start_high:
            out.append("Awake until gpio 15 goes low\n")
        out.append(asleep + "\n")
    return "".join(out)


@pytest.mark.parametrize("start_high", (False, True))
def test_the_low_power_gpio_programs_cycle_on_the_wake_wave(start_high):
    judged("low_power_dormant_gpio", low_power_run("d", "Dormant until gpio 15 goes high", start_high))
    judged("low_power_sleep_gpio", low_power_run("s", "Sleeping until gpio 15 goes high", start_high))
    judged("low_power_pstate_gpio", low_power_run("p", "Low power until gpio 15 goes high", start_high, boot=True))
    stuck = low_power_run("d", "Dormant until gpio 15 goes high", start_high, runs=1)
    assert spec("low_power_dormant_gpio").match(stuck)[0]


def test_the_wake_wave_dips_briefly_once_a_second():
    (wake,) = PARTS["low_power_dormant_gpio"]
    period, high = wake.properties["period"], wake.properties["high"]
    assert period == 1_000_000_000 and period - high == 100_000_000
    assert all(PARTS[t] == (wake,) for t in ("low_power_sleep_gpio", "low_power_pstate_gpio"))


@pytest.mark.parametrize("first", ("RISE", "FALL"))
def test_hello_gpio_irq_prints_each_edge(first):
    other = {"RISE": "FALL", "FALL": "RISE"}
    lines, edge = ["Hello GPIO IRQ\n"], first
    for _ in range(8):
        lines.append(f"GPIO 2 EDGE_{edge}\n")
        edge = other[edge]
    judged("hello_gpio_irq", "".join(lines))
    assert spec("hello_gpio_irq").match("Hello GPIO IRQ\nGPIO 2 EDGE_FALL, EDGE_RISE\n" * 4)[1]


def joystick_line(x, y):
    bar = lambda raw: "".join("o" if i == raw * 40 // 4095 else " " for i in range(40))
    return f"\rX: [{bar(x)}]  Y: [{bar(y)}]"


@pytest.mark.parametrize("scale", (0.97, 1.0, 1.03))
def test_joystick_display_shows_the_pwm_levels_on_the_carrier(scale):
    x, y = round(4095 * 38 / 150 * scale), min(4095, round(4095 * 113 / 150 * scale))
    judged("joystick_display", joystick_line(x, y) * 4, True, METALCLAW_PICO_RP2_FIXTURE["analog_pins"])


@pytest.mark.parametrize("y", (4095, 4090))
def test_joystick_display_shows_the_rails_on_fixture_1(y):
    judged("joystick_display", joystick_line(3, y) * 4, False, FIXTURE["analog_pins"])
    assert spec("joystick_display").for_bench(False, ()).match(joystick_line(1024, 3071) * 4)[0]


def test_microphone_adc_prints_the_level():
    for scale in (0.97, 1.0, 1.03):
        raw = round(4095 * 0.4 * scale)
        text = "Beep boop, listening...\n" + f"{raw * 3.3 / 4095:.2f}\n" * 6
        judged("microphone_adc", text, True, (26, 27, 28))
    judged("microphone_adc", "Beep boop, listening...\n" + "0.00\n0.01\n" * 3, False, ())


# ---- jumpers -----------------------------------------------------------


def test_pwm_measure_duty_cycle_reads_back_what_it_drives():
    # measure_duty_cycle.c:45-72: level = duty x (1000 + 1) truncated; the
    # counter counts high cycles / 100 over sleep_ms(10), a little more.
    lines = ["\nPWM duty cycle measurement example\n"]
    for duty in (0.0, 0.1, 0.5, 0.9, 1.0):
        level = int(duty * 1001)
        measured = int(125e6 * 0.01 * 1.0004 * level / 1001 / 100) / 12500
        lines.append(f"Output duty cycle = {duty * 100:.1f}%, measured input duty cycle = {measured * 100:.1f}%\n")
    judged("pwm_measure_duty_cycle", "".join(lines))
    unjumpered = "".join(l if "Output" not in l else l.split("measured")[0] + "measured input duty cycle = 0.0%\n"
                         for l in lines)
    assert spec("pwm_measure_duty_cycle").match(unjumpered)[0]


@pytest.mark.parametrize("target, file", (("slave_mem_i2c", "split"), ("slave_mem_i2c_burst", "burst")))
def test_slave_mem_i2c_writes_and_reads_back(target, file):
    lines = ["\nI2C slave example\n"]
    for a in range(0, 256, 32)[:4]:
        msg = f"Hello, I2C slave! - 0x{a:02X}"
        lines.append(f"Write at 0x{a:02X}: '{msg}'\n")
        if file == "split":
            lines += [f"Read  at 0x{a:02X}: '{msg[:5]}'\n", f"Read  at 0x{a + 5:02X}: '{msg[5:]}'\n"]
        else:
            lines.append(f"Read  at 0x{a:02X}: '{msg}'\n")
        lines.append("\n")
    judged(target, "".join(lines))
    assert spec(target).match("\nI2C slave example\nCouldn't write to slave, please check your wiring!\n")[1]


def test_the_pio_jumper_programs_print_their_words():
    for target in ("pio_manchester_encoding", "pio_differential_manchester"):
        judged(target, "Transmit program loaded at 0\nReceive program loaded at 3\n00000000\n0ff0a55a\n12345678\n")
    judged("pio_uart_dma", "pio_rx dma_rx=1/43\ndma_tx done\npio_rx dma_rx=40/43\ndma_rx done\nTest passed\n")
    judged("pio_uart_rx", "Starting PIO UART RX example\n"
                          "Hello, world from PIO! (Plus 2 UARTs and 2 cores, for complex reasons)\n")
    judged("pio_uart_rx_intr", "Starting PIO UART RX interrupt example\n"
           + "".join(f"Hello, world from PIO with interrupts! {n}\n" for n in range(10)) + "Test complete\n")
    data = ["67", "c6", "69", "73", "51", "ff", "4a", "ec"]
    judged("pio_clocked_input", "SPI running at 20833333 Hz\nData to transmit:\n" + "".join(f"{d}\n" for d in data)
           + "Reading back from RX FIFO:\n" + "".join(f"{d} OK\n" for d in data) + "Done.\n")


# ---- pins --------------------------------------------------------------


def sampled(level_at, seconds, step=0.003, pins_driven=(25,)):
    driven = sum(1 << p for p in pins_driven)
    return [pins.Sample(round(t * step * 1e6), level_at(t * step), driven) for t in range(int(seconds / step))]


def blinky_levels(phase):
    # picoboard/blinky/blinky.c:57-87: the LED's (level, ms) timeline of one
    # "Hello world" and the 1 s after it, from `phase` seconds into it.
    morse = {"H": "....", "E": ".", "L": ".-..", "O": "---", "W": ".--", "R": ".-.", "D": "-.."}
    timeline = []
    for c in "Hello world":
        if c == " ":
            timeline.append((0, 400))
            continue
        for e in morse[c.upper()]:
            timeline += [(1, 100 if e == "." else 300), (0, 100)]
        timeline.append((0, 200))
    timeline.append((0, 1000))
    total = sum(ms for _, ms in timeline) / 1000

    def level(t):
        t = (t + phase) % total
        for lv, ms in timeline:
            if t < ms / 1000:
                return lv << 25
            t -= ms / 1000
        return 0

    return level, total


@pytest.mark.parametrize("phase", (0.0, 3.3, 9.95, 12.0))
def test_blinky_keys_hello_world(phase):
    level, total = blinky_levels(phase)
    assert 12 < total < 13
    spec = PIN_SPECS["picoboard_blinky"]
    samples = sampled(level, spec.seconds)
    assert [p for c in spec.toggles for p in c.check(samples)] == []
    assert "HELLO WORLD" in pins.morse(samples, 25, 0.1)


def seven_segment(button_wave, phase=0.0):
    # hello_7segment.c:57-81: every 250 ms the digit steps down, or up while
    # GP9 is low, and the segments show it lit low.
    digit, shown, steps = 0, [], []
    t = 0.0
    while t < 12:
        pressed = not button_wave(t + phase)
        digit = (digit + 1) % 10 if pressed else (digit - 1) % 10
        steps.append((t, digit))
        t += 0.25

    def level(at):
        d = [dg for st, dg in steps if st <= at][-1]
        segments = pins.SEGMENT_DIGITS[d] << 2
        return (~segments & (0x7F << 2)) | (int(button_wave(at + phase)) << 9)

    return level


def test_hello_7segment_counts_with_the_button():
    (button,) = PARTS["hello_7segment"]
    period, high = button.properties["period"] / 1e9, button.properties["high"] / 1e9
    wave = lambda t: (t % period) < high
    spec = PIN_SPECS["hello_7segment"]
    for phase in (0.0, 1.1, 2.6):
        samples = sampled(seven_segment(wave, phase), spec.seconds, pins_driven=range(2, 9))
        assert [p for c in spec.toggles for p in c.check(samples)] == [], phase
    stuck = sampled(seven_segment(lambda t: True), spec.seconds, pins_driven=range(2, 9))
    assert [p for c in spec.toggles for p in c.check(stuck)]


def test_hello_pio_stops_after_the_key():
    # hello.c:48-55: put 1, 500 ms, put 0, 500 ms, then look for a key; one
    # at 3 s is seen by the look at 4 s at the latest, after the fall at 3.5.
    spec = PIN_SPECS["hello_pio"]
    ((at, key),) = spec.send
    blink = lambda t: (int(t / 0.5) % 2 == 0 and t < at + 1.0) << 25
    assert [p for c in spec.toggles for p in c.check(sampled(blink, spec.seconds))] == []
    never = lambda t: (int(t / 0.5) % 2 == 0) << 25
    assert [p for c in spec.toggles for p in c.check(sampled(never, spec.seconds))]


def test_the_button_press_shows_on_the_led():
    spec = PIN_SPECS["picoboard_button"]
    pressed = lambda t: any(p.at + 0.012 <= t < p.release + 0.012 for p in spec.presses)
    assert [p for c in spec.toggles for p in c.check(sampled(lambda t: pressed(t) << 25, spec.seconds))] == []
    assert [p for c in spec.toggles for p in c.check(sampled(lambda t: 0, spec.seconds))]


class FakeLink:
    def __init__(self, chip="rp2040", step_us=2000, stall_after=None):
        self.timer, self.gpio_in, self.io_bank0 = pins.CHIPS[chip]
        self.now = 0
        self.step = step_us
        self.stall_after = stall_after
        self.writes = []

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read_words(self, address, count):
        if address == self.timer:
            if self.stall_after is None or self.now < self.stall_after:
                self.now += self.step
            return [self.now]
        if address == pins.USB_MAIN_CTRL:
            return [1]
        if address == pins.USB_SIE_CTRL:
            return [1 << 16]
        return [pins.STATUS_OETOPAD if address >= self.io_bank0 and address != self.gpio_in else 0]

    def write_words(self, address, values):
        self.writes.append((self.now, address, values))


def test_the_watch_runs_its_actions_at_their_times_and_presses_go_through_the_aliases():
    spec = PinSpec(3.0, (), presses=(Press(1.0, 2.0),), send=((0.5, b"q"),))
    port = FakePort()
    link = FakeLink()
    pins.watch(None, "rp2040", spec.seconds, (25,), link=link, actions=pin_actions(spec, "rp2040", port))
    assert port.written == [b"q"]
    ss = pins.QSPI_SS_CTRL["rp2040"]
    assert [(round(t / 1e6, 1), a, v) for t, a, v in link.writes] == [
        (1.0, ss + pins.ALIAS_SET, [1 << 17]), (2.0, ss + pins.ALIAS_CLR, [1 << 17])]
    assert pins.QSPI_SS_CTRL["rp2350"] == 0x4003001C and pins.usb_presenting(link, "rp2040")


def test_a_watch_whose_dut_timer_stops_ends(monkeypatch):
    clock = itertools.count()
    monkeypatch.setattr(pins, "time", types.SimpleNamespace(monotonic=lambda: next(clock) * 0.5))
    samples = pins.watch(None, "rp2350", 60.0, (), link=FakeLink("rp2350", stall_after=100_000))
    assert samples[-1].us == 98_000


def test_run_resets_pulses_nreset_with_the_probes_own_waits(monkeypatch):
    sent = []

    class Link:
        def __init__(self, *args, **kwargs):
            assert kwargs == {"connect": False}

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def _command(self, packet):
            sent.append(packet)
            return packet[:1] + b"\x00"

    monkeypatch.setattr(dap, "_Link", Link)
    dap.run_resets("SERIAL", 2, 10_000, 100_000)
    assert [p.hex() for p in sent] == ["100080" + "10270000", "108080" + "a0860100",
                                       "100080" + "10270000", "108080" + "00000000"]


def test_the_double_reset_lands_inside_the_window():
    s = BOOTSEL_SPECS["hello_double_tap"]
    # PICO_BOOTSEL_VIA_DOUBLE_RESET_TIMEOUT_MS 200 from when the program
    # reaches its check; the second reset comes gap_s after the first.
    assert 0.03 <= s.gap_s <= 0.15 and s.low_s < s.gap_s
    blink = sampled(lambda t: (int(t / 0.25) % 2) << 25, s.seconds)
    assert len(pins.edges(blink, s.led)) >= s.edges


def test_toggle_until_allows_no_change_after_it():
    t = pins.Toggle(25, 4, (0.45, 0.55), until=3.0)
    assert t.check(sampled(lambda x: (int(x / 0.5) % 2 == 1 and x < 2.5) << 25, 5.0)) == []
    assert t.check(sampled(lambda x: (int(x / 0.5) % 2) << 25, 5.0))


def test_spec_for_bench_takes_the_analog_variant_only_where_needed():
    s = Spec(console=("a",), analog_pins=(26,), without_analog={"console": ("b",)})
    assert s.for_bench(False, (26, 27)).console == ("a",)
    assert s.for_bench(False, ()).console == ("b",)
    assert s.for_bench(True, ()).console == ("b",)


def test_a_restart_that_lands_on_a_sleeping_program_waits_for_it_to_wake():
    from test.rig.platforms.hardware.platform import Example
    from test.rig.platforms.hardware.probe import ProbeError

    calls = []

    class Asleep(Example):
        def __init__(self):
            self.ram_only = False
            self.resets = 0

        def reset(self):
            self.resets += 1
            calls.append("reset")
            if self.resets == 1:
                raise ProbeError("the DUT answers WAIT")

        def halt_awake(self, wake_harness_pin, timeout=None):
            calls.append(("halt", wake_harness_pin, timeout))

    Asleep().restart()
    assert calls == ["reset", ("halt", None, Example.ASLEEP_MAX_S), "reset"]
    assert Example.ASLEEP_MAX_S > 10.0


# ---- counts and margins ------------------------------------------------


def blink_from(first, half=0.25, seconds=3.0, undriven_after=None):
    # The LED on at `first` s and toggling every `half`, driven once on.
    def level(t):
        return (t >= first and int((t - first) / half) % 2 == 0) << 25
    out = []
    for s in sampled(level, seconds):
        t = s.us / 1e6
        driven = t >= first and not (undriven_after is not None and undriven_after <= t < undriven_after + 0.05)
        out.append(pins.Sample(s.us, s.levels, int(driven) << 25))
    return out


def test_blink_needs_the_changes_its_period_makes_in_the_watch():
    (check,) = PIN_SPECS["blink"].toggles
    # blink.c:46-51 at LED_DELAY_MS 250: 12 changes in 3 s.
    assert check.edges == 10
    for first in (0.0, 0.01, 0.2):
        assert check.check(blink_from(first)) == [], first
    # Starting 0.75 s late leaves 9.
    assert any("changed 9 times" in p for p in check.check(blink_from(0.75)))


def test_a_pin_must_be_driven_from_its_first_change_on():
    (check,) = PIN_SPECS["blink"].toggles
    assert check.check(blink_from(0.1)) == []
    assert any("not driven" in p for p in check.check(blink_from(0.1, undriven_after=0.6)))
    assert any("not driven" in p for p in check.check(blink_from(0.1, undriven_after=2.6)))


def test_the_pio_blink_counts_follow_each_pins_frequency():
    spec = PIN_SPECS["pio_blink"]
    assert [(c.pin, c.edges) for c in spec.toggles] == [(3, 30), (4, 22), (5, 14), (6, 6)]


def test_a_count_that_goes_the_wrong_way_after_a_button_edge_fails():
    (button,) = PARTS["hello_7segment"]
    period, high = button.properties["period"] / 1e9, button.properties["high"] / 1e9
    spec = PIN_SPECS["hello_7segment"]
    (check,) = spec.toggles
    assert (check.steps, check.each_way) == (34, 14)

    def late(t):
        # The button as a program that read it 50 ms late would see it: the
        # step 20 ms after each edge goes the old way.
        return ((t - 0.05) % period) < high
    shifted = sampled(seven_segment(late, 0.02), spec.seconds, pins_driven=range(2, 9))
    honest = [pins.Sample(s.us, (s.levels & ~(1 << 9)) | (int(((s.us / 1e6 + 0.02) % period) < high) << 9), s.driven)
              for s in shifted]
    assert any("with GP9" in p for p in check.check(honest))


def test_line_inputs_wait_for_a_shell_prompt_through_its_colours():
    port = FakePort()
    t = Typist(port, ((r"^uart:~\$ $", b"kernel version\r"), (r"^uart:~\$ $", b"kernel cycles\r")), lines=True)
    t("*** Booting Zephyr OS build v4.4.0 ***\r\n\x1b[1;32muart:~")
    assert port.written == []
    text = "*** Booting Zephyr OS build v4.4.0 ***\r\n\x1b[1;32muart:~$ \x1b[m"
    t(text)
    assert port.written == [b"kernel version\r"]
    text += "kernel version\r\nZephyr version 4.4.0\r\n"
    t(text)
    assert port.written == [b"kernel version\r"]
    t(text + "\x1b[1;32muart:~$ \x1b[m")
    assert port.written == [b"kernel version\r", b"kernel cycles\r"]
