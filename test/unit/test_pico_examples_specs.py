# SPDX-License-Identifier: Apache-2.0 OR MIT

import json
import re

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
