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
