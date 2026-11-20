# SPDX-License-Identifier: Apache-2.0 OR MIT

import json
import re
import types

import pytest

from test.pico_examples.targets import SPECS, Spec
from test.rig.platforms.hardware import platform


def test_expected_lines_match_in_order():
    spec = Spec(console=("one", "two"))
    assert spec.match("one two") == ([], [])
    assert spec.match("two one") == (["two"], [])


def test_a_repeated_line_needs_each_repeat():
    spec = Spec(console=("tick\n",) * 3)
    assert spec.match("tick\ntick\n")[0] == ["tick\n"]
    assert spec.match("tick\ntick\ntick\n")[0] == []


def test_a_panic_is_found_anywhere():
    spec = Spec(console=("ok",))
    assert spec.match("ok\n*** PANIC ***\n")[1] == [r"\*\*\* PANIC \*\*\*"]


def test_every_spec_names_its_source():
    assert all(spec.source for spec in SPECS.values())


def test_every_pattern_compiles_on_every_board():
    for spec in SPECS.values():
        for board in ("pico", "pico2"):
            judged = spec.for_board(board)
            for pattern in (judged.console + judged.forbidden) if judged else ():
                re.compile(pattern)


def test_a_spec_limited_to_some_boards_judges_no_other():
    spec = Spec(console=("ok",), boards=("pico2",))
    assert spec.for_board("pico") is None
    assert spec.for_board("pico2").console == ("ok",)


def test_a_board_override_replaces_only_its_fields():
    spec = Spec(console=("both",), seconds=3.0, per_board={"pico": {"console": ("rp2040",)}})
    assert spec.for_board("pico").console == ("rp2040",)
    assert spec.for_board("pico").seconds == 3.0
    assert spec.for_board("pico2").console == ("both",)


def test_freertos_panics_only_count_before_main_task_exits():
    spec = SPECS["hello_freertos_one_core"]
    lines = "Starting FreeRTOS on core 0:\n" + "".join(
        f"Hello from main task count={n}\n" + ("Hello from worker count=1\n" if n == 3 else "")
        + ("Hello from worker count=2\n" if n == 6 else "") for n in range(10))
    exited = lines + "main_task is exiting now\n"
    assert spec.match(exited) == ([], [])
    assert spec.match(exited + "\n*** PANIC ***\n\nnot supported\n")[1] == []
    assert spec.match(lines + "\n*** PANIC ***\n")[1] != []


def test_an_example_image_is_known_by_its_metadata(tmp_path):
    elf = tmp_path / "hello_serial.elf"
    elf.write_bytes(b"")
    assert platform.example_metadata(elf) is None
    (tmp_path / "example.json").write_text(json.dumps({"target": "hello_serial", "chip": "rp2350"}))
    assert platform.example_metadata(elf)["target"] == "hello_serial"
    assert platform.soc_series(elf) == "rp2350"


def test_rig_cases_skip_on_an_example_and_example_cases_on_a_rig_image(tmp_path):
    elf = tmp_path / "zephyr.elf"
    elf.write_bytes(b"")
    with pytest.raises(pytest.skip.Exception):
        platform.require_example_image(elf)
    platform.require_rig_image(elf)
    (tmp_path / "example.json").write_text("{}")
    with pytest.raises(pytest.skip.Exception):
        platform.require_rig_image(elf)


def test_a_bench_with_line_pullups_takes_its_own_fields():
    spec = Spec(console=("a",), with_line_pullups={"console": ("b",)})
    assert spec.for_bench(True).console == ("b",)
    assert spec.for_bench(False).console == ("a",)


def test_pio_spi_flash_is_not_judged_on_fixture_1s_pico_2_build_only():
    from test.pico_examples.hazards import BENCH_LIMITS, bench_limit
    from test.pico_examples.part_targets import PART_SPECS
    for board in ("fixture1_pico", "rpi_pi_pico"):
        reason = bench_limit("pio_spi_flash", board, "pico2")
        assert "7-13 ns" in reason and "tSHSL2" in reason and "3 of 15" in reason
    assert bench_limit("pio_spi_flash", "metalclaw_pico_rp2", "pico") is None
    assert bench_limit("pio_spi_flash", "rpi_pi_pico", "pico") is None
    assert bench_limit("spi_flash", "rpi_pi_pico", "pico2") is None
    assert set(BENCH_LIMITS) <= set(SPECS) | set(PART_SPECS)


def test_pio_manchester_encoding_is_not_judged_on_the_carrier():
    from test.pico_examples.hazards import bench_limit
    reason = bench_limit("pio_manchester_encoding", "metalclaw_pico_rp2", "pico")
    assert "48 ns half-bit" in reason and "2 of 12" in reason
    for board in ("fixture1_pico", "rpi_pi_pico"):
        assert bench_limit("pio_manchester_encoding", board, "pico2") is None
    assert bench_limit("pio_differential_manchester", "metalclaw_pico_rp2", "pico") is None


def test_the_hardware_example_fixture_skips_a_bench_limit_before_flashing(tmp_path, monkeypatch):
    image = tmp_path / "pio_spi_flash.elf"
    image.touch()
    (tmp_path / "example.json").write_text(json.dumps({"target": "pio_spi_flash", "board": "pico2",
                                                       "chip": "rp2350"}))
    monkeypatch.setenv("METALCLAW_DUT_IMAGE", str(image))
    harness = type("H", (), {"board": lambda self: "fixture1_pico"})()
    example = platform.example
    function = getattr(example, "_get_wrapped_function", lambda: example.__wrapped__)()
    with pytest.raises(pytest.skip.Exception, match="pio_spi_flash's pico2 build holds chip select high"):
        function(harness, None, {}, None)


# otp/hello_otp/hello_otp.c on blank rows (:42-185), as the bench's boot run
# prints it, and on the rows that run leaves, as every later run does.
HELLO_OTP_FIRST = ("ECC Write succeeded\nECC Data read is \"Hello from OTP\"\nRaw overwrite of ECC data succeeded\n"
                   "Raw Write succeeded\nOTP Software Lock Done\nLocked read failed as expected\n")
HELLO_OTP_LATER = ("ERROR: This example requires empty OTP rows to run - change the ecc_row and raw_row variables to an "
                   "empty row and recompile\n")


def test_hello_otp_is_judged_on_the_run_after_the_one_that_wrote_its_rows():
    spec = SPECS["hello_otp"]
    assert spec.match(HELLO_OTP_LATER) == ([], [])
    assert spec.match(HELLO_OTP_FIRST)[0] and spec.match(HELLO_OTP_FIRST)[1]
    assert spec.match("ERROR: Initial ECC Row Read failed with error -4\n" + HELLO_OTP_LATER)[1]


def lines_of(spec):
    # The text a Spec built from whole lines expects, one line each.
    return "".join(re.sub(r"\\(.)", r"\1", p.replace("(?m)^", "").replace(r"\r?\n", "\n")) for p in spec.console)


# RP2350 datasheet 3.1.10.2-4 prints these for hello_interp.c.
def test_hello_interp_expects_the_datasheets_blends_clamp_and_interpolation():
    text = lines_of(SPECS["hello_interp"])
    assert SPECS["hello_interp"].match(text) == ([], [])
    assert ("unsigned:\n0xfffffc18\n0xd5fffd60\n0xaafffeb0\n0x80fffff8\n0x56000148\n0x2c000290\n0x010003e0\n"
            "Simple blend 3:\n0x00004000\n0x0000e800\n0xffffe800\n") in text
    assert "1024\t255\nLinear interpolation:\n0\t(0% between 0 and 10)\n2\t(25% between 0 and 10)\n" in text
    assert "-13\t(75% between 10 and -20)\n" in text and "125\t(75% between -1000 and 500)\n" in text
    assert text.endswith("0x20\n0x20\n0x31\n0x31\n")
    assert SPECS["hello_interp"].match(text.replace("\n582\n", "\n583\n"))[0]


def test_hello_dcp_expects_its_exact_butterflies_and_the_float_iir():
    text = lines_of(SPECS["hello_dcp"])
    assert SPECS["hello_dcp"].match(text) == ([], [])
    assert "=(7+10j,-3-4j)\n" in text and "=(7+10j,5.5-13.5j)\n" in text and "=(-8+26j,12-20j)\n" in text
    assert "=(59+70j,-33-30j,-45+22j,27-50j)\n" in text
    # Octave's filter(b, a, [1 zeros(1,19)]) to %g's six figures, which the
    # float filter keeps for the first samples.
    assert "y[ 0]=0.307043\ny[ 1]=0.594416\n" in text


def test_pio_addition_expects_newlibs_first_twenty_rand_values_added():
    text = lines_of(SPECS["pio_addition"])
    assert text.startswith("Doing some random additions:\n33 + 43 = 76\n62 + 29 = 91\n")
    assert text.endswith("69 + 32 = 101\n") and text.count("\n") == 11
    assert SPECS["pio_addition"].match(text.replace("= 76", "= 77"))[0]


def test_multicore_doorbell_prints_a_count_and_its_own_percentage():
    spec = SPECS["multicore_doorbell"]
    for count in (1, 999, 1000, 54321, 99999, 100000):
        assert spec.match(f"core1 is exiting\nFinal doorbell irq count {count}/100000 ({count // 1000}%)\n") == (
            [], []), count
    for count, percent in ((0, 0), (54321, 5), (100001, 100), (999, 1)):
        assert spec.match(f"core1 is exiting\nFinal doorbell irq count {count}/100000 ({percent}%)\n")[0], count


class Bench:
    # A console on which `lines` come at their bench times; a read waits up
    # to 0.1 s for them, a clock reading takes `round_trip`.
    def __init__(self, lines, round_trip=0.02):
        self.now, self.lines, self.round_trip = 0.0, list(lines), round_trip

    def clock(self):
        self.now += self.round_trip / 2
        t = self.now
        self.now += self.round_trip / 2
        return t

    def read(self, size):
        due = [text for t, text in self.lines if t <= self.now + 0.1]
        if due:
            at = max(t for t, text in self.lines if t <= self.now + 0.1)
            self.now = max(self.now, at)
            self.lines = [(t, text) for t, text in self.lines if t > self.now]
            return "".join(due).encode()
        self.now += 0.1
        return b""


def timed(target, times, line="Hello, world!\n"):
    from test.pico_examples import cases
    spec = SPECS[target]
    bench = Bench([(t, line) for t in times])
    arrivals = []
    text = cases.read_console(bench, bench, spec, arrivals=arrivals)
    assert spec.match(text) == ([], []), text
    return spec.timing(text, arrivals)


def test_a_program_that_prints_every_second_is_timed_by_the_bench_clock():
    assert SPECS["hello_serial"].intervals == ((0, 1, 1.0, 1.001), (1, 2, 1.0, 1.001))
    assert timed("hello_serial", (0.3, 1.3, 2.3)) == []
    assert timed("hello_serial", (0.3, 1.31, 2.31)) == []
    assert timed("hello_serial", (0.3, 0.6, 0.9))
    assert timed("hello_serial", (0.3, 2.3, 4.3))


def test_a_spec_without_intervals_is_not_timed():
    assert SPECS["hello_reset"].intervals == () and SPECS["hello_reset"].timing("Hello, reset!\n", []) == []


def test_every_interval_names_two_console_patterns_in_order():
    from test.pico_examples.part_targets import PART_SPECS
    for target, spec in {**SPECS, **PART_SPECS}.items():
        for i, j, least, most in spec.intervals:
            assert 0 <= i < j < len(spec.console) and 0 < least <= most, target


def test_a_double_resets_second_press_lands_inside_the_programs_window():
    from test.pico_examples.pin_specs import BOOTSEL_SPECS
    # PICO_BOOTSEL_VIA_DOUBLE_RESET_TIMEOUT_MS is 200.
    assert BOOTSEL_SPECS["hello_double_tap"].gap_s < 0.2 < BOOTSEL_SPECS["hello_double_tap"].window_s


def test_every_reset_of_a_double_reset_program_waits_out_its_window(monkeypatch):
    from test.rig.platforms.hardware import platform as hardware
    events = []
    e = object.__new__(hardware.Example)
    e.harness = types.SimpleNamespace(sleep=lambda s: events.append(("sleep", s)), serial="X",
                                      run=lambda code: events.append("release"))
    e.ram_only, e.reset_window_s = False, 0.3
    e.reset = lambda: events.append("reset")
    monkeypatch.setattr(hardware.dap, "run_resets", lambda *a: events.append("run"))
    e.restart()
    e.reset_by_run(2, 0.01, 0.1)
    assert events == [("sleep", 0.3), "reset", ("sleep", 0.3), "run", "release"]
    e.reset_window_s, events[:] = 0.0, []
    e.restart()
    assert events == ["reset"]
