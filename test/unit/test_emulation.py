# SPDX-License-Identifier: Apache-2.0 OR MIT
# What the pico-examples programs need of the emulated bench, and how the
# emulator platform applies it and reports stand-ins.

import types
from decimal import Decimal

from test.pico_examples import emulation as emu
from test.pico_examples.emulation import EMULATION, Emulation, StandIn, emulation_for
from test.pico_examples.parts import JUMPERS, PARTS, parts_for
from test.rig.platforms.emulator import platform


def test_every_need_keeps_the_latency_above_two_quanta_in_whole_microseconds():
    for target, e in EMULATION.items():
        quantum = Decimal(e.quantum or "0.0001")
        latency = 300 if e.wire_latency_us is None else e.wire_latency_us
        assert isinstance(latency, int) and not isinstance(latency, bool) and latency > 0, target
        assert Decimal(latency) / 1_000_000 > 2 * quantum, target
        assert e.i2c0_wiring in (None, "bus", "wires"), target


def test_every_harness_relayed_jumper_runs_at_a_fine_quantum():
    relayed = {t for t, parts in JUMPERS.items() if all(p.model == "jumper" for p in parts)}
    assert relayed
    for target in relayed:
        assert EMULATION[target] == Emulation(quantum="0.000001", wire_latency_us=3), target


def test_a_stand_in_replaces_a_part_of_its_program_on_the_same_pins():
    for target, e in EMULATION.items():
        parts = {p.name: p for p in PARTS[target]}
        for s in e.stand_ins:
            assert s.replaces in parts, target
            assert set(parts[s.replaces].dut_pins()) == {s.a, s.b}, target


def test_the_i2c_pair_is_two_stand_ins_on_pin_level_wires():
    for target in ("slave_mem_i2c", "slave_mem_i2c_burst"):
        e = EMULATION[target]
        assert e.i2c0_wiring == "wires"
        assert [s.wire for s in e.stand_ins] == ["jumper_gp4_gp6", "jumper_gp5_gp7"]
        assert e.stand_in_label == "stand-in-jumper_gp4_gp6+jumper_gp5_gp7"


def test_a_program_without_needs_and_a_rig_image_get_the_defaults():
    assert emulation_for({"target": "hello_serial"}) is emu.DEFAULT
    assert emulation_for(None) is emu.DEFAULT
    assert emu.DEFAULT.stand_in_label == ""


# ---- reporting -------------------------------------------------------------


class Metafunc:
    def __init__(self, fixturenames):
        self.fixturenames = fixturenames
        self.calls = []

    def parametrize(self, *args, **kwargs):
        self.calls.append((args, kwargs))


STAND_INS = Emulation(stand_ins=(StandIn(4, 6, "sda"), StandIn(5, 7, "scl")))


def test_a_session_with_stand_ins_names_every_result_after_them(monkeypatch):
    monkeypatch.setattr(platform, "image_emulation", lambda: STAND_INS)
    m = Metafunc(["harness", "emulation", "example"])
    platform.pytest_generate_tests(m)
    ((args, kwargs),) = m.calls
    assert args == ("emulation", [STAND_INS])
    assert kwargs == {"ids": ["stand-in-jumper_gp4_gp6+jumper_gp5_gp7"], "indirect": True, "scope": "session"}


def test_a_session_without_stand_ins_keeps_its_names(monkeypatch):
    monkeypatch.setattr(platform, "image_emulation", lambda: Emulation(quantum="0.000001", wire_latency_us=3))
    m = Metafunc(["emulation"])
    platform.pytest_generate_tests(m)
    assert m.calls == []
    monkeypatch.setattr(platform, "image_emulation", lambda: STAND_INS)
    m = Metafunc(["harness"])
    platform.pytest_generate_tests(m)
    assert m.calls == []


def test_a_result_with_stand_ins_carries_them_as_a_property():
    stood_in = types.SimpleNamespace(callspec=types.SimpleNamespace(params={"emulation": STAND_INS}),
                                     user_properties=[])
    plain = types.SimpleNamespace(user_properties=[])
    platform.pytest_collection_modifyitems([stood_in, plain])
    assert stood_in.user_properties == [("emulator_stand_ins", "jumper_gp4_gp6 jumper_gp5_gp7")]
    assert plain.user_properties == []


def test_the_session_emulation_comes_from_the_dut_images_program(tmp_path, monkeypatch):
    image = tmp_path / "slave_mem_i2c.elf"
    image.touch()
    (tmp_path / "example.json").write_text('{"target": "slave_mem_i2c", "board": "pico", "chip": "rp2040"}')
    monkeypatch.setenv("METALCLAW_DUT_IMAGE", str(image))
    assert platform.image_emulation() is EMULATION["slave_mem_i2c"]


def test_no_dut_image_gives_the_defaults(tmp_path, monkeypatch):
    monkeypatch.setenv("METALCLAW_DUT_IMAGE", str(tmp_path / "missing.elf"))
    assert platform.image_emulation() is emu.DEFAULT


def test_the_emulation_fixture_takes_the_parametrized_value(monkeypatch):
    monkeypatch.setattr(platform, "image_emulation", lambda: emu.DEFAULT)
    assert platform.session_emulation(STAND_INS) is STAND_INS
    assert platform.session_emulation(None) is emu.DEFAULT


def test_a_part_a_stand_in_replaces_is_not_attached():
    assert [p.name for p in parts_for("slave_mem_i2c", "pico")] == ["sda", "scl"]
    replaced = {s.replaces for s in EMULATION["slave_mem_i2c"].stand_ins}
    assert parts_for("slave_mem_i2c", "pico", replaced) == ()
    assert [p.name for p in parts_for("slave_mem_i2c", "pico", {"sda"})] == ["scl"]
