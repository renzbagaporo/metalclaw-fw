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


def test_every_harness_relayed_jumper_runs_at_a_fine_quantum_unless_stood_in():
    relayed = {t for t, parts in JUMPERS.items() if all(p.model == "jumper" for p in parts)}
    assert relayed
    for target in relayed:
        e = EMULATION[target]
        if e.stand_ins:
            continue
        assert (e.quantum, e.wire_latency_us) == ("0.000001", 3), target


def test_a_relayed_program_on_gp4_or_gp5_takes_the_pin_level_i2c0_wires():
    for target in ("pio_uart_rx", "pio_uart_rx_intr", "pio_uart_dma", "pwm_measure_duty_cycle"):
        assert {4, 5} & {p for part in PARTS[target] for p in part.dut_pins()}, target
        assert EMULATION[target] == Emulation(quantum="0.000001", wire_latency_us=3, i2c0_wiring="wires"), target
    assert EMULATION["pio_uart_rx"] is emu.RELAYED_ON_GP4_GP5


def test_the_1_wire_and_dht_parts_answer_over_relay_wires():
    for target in ("pio_onewire", "dht"):
        assert EMULATION[target] is emu.RELAYED, target
        assert EMULATION[target].i2c0_wiring is None


def test_a_stand_in_replaces_a_part_of_its_program_on_the_same_pins():
    for target, e in EMULATION.items():
        parts = {p.name: p for p in PARTS.get(target, ())}
        for s in e.stand_ins:
            if s.replaces is None:
                continue
            assert s.replaces in parts, target
            assert set(parts[s.replaces].dut_pins()) == set(s.pins), target


def test_no_harness_part_left_attached_takes_a_stand_ins_pins():
    for target, e in EMULATION.items():
        for board in ("pico", "pico2"):
            assert platform.stand_in_clashes(target, board, e) == [], (target, board)


def test_a_stand_in_clash_names_the_part_and_its_pins():
    assert platform.stand_in_clashes("pio_clocked_input", "pico", Emulation(stand_ins=(StandIn(2, 5, "clock"),))) == []
    e = Emulation(stand_ins=(StandIn(2, 4, "clock"),))
    assert platform.stand_in_clashes("pio_clocked_input", "pico", e) == ["data on GP[4]"]
    flash = Emulation(stand_ins=(emu.FlashStandIn(17, 18, 19, 16, "other"),))
    assert platform.stand_in_clashes("pio_spi_flash", "pico", flash) == ["flash on GP[16, 17, 18, 19]"]


def test_pio_clocked_input_runs_on_two_wires_inside_the_dut():
    e = EMULATION["pio_clocked_input"]
    assert [(s.a, s.b, s.replaces) for s in e.stand_ins] == [(2, 5, "clock"), (3, 4, "data")]
    assert e.stand_in_label == "stand-in-jumper_gp2_gp5+jumper_gp3_gp4"
    # Neither GP4 nor GP5 reaches the harness: the default I2C0 bus leaves
    # their pads unwired, and nothing crosses the boards.
    assert (e.i2c0_wiring, e.quantum, e.wire_latency_us) == (None, None, None)
    assert parts_for("pio_clocked_input", "pico", {s.replaces for s in e.stand_ins}) == ()


def test_pio_spi_flash_runs_against_a_flash_inside_the_dut():
    e = EMULATION["pio_spi_flash"]
    (flash,) = e.stand_ins
    assert flash.replaces == "flash" and flash.renode_type == "RP2_PinSPIFlash"
    assert e.stand_in_label == "stand-in-pin_spi_flash_gp17"
    assert flash.monitor_commands("pico", {}) == (
        'mach set "pico"',
        'machine LoadPlatformDescriptionFromString "spiFlash: SPI.RP2_PinSPIFlash @ none { gpio: gpio; '
        'csPin: 17; sckPin: 18; mosiPin: 19; misoPin: 16 }"')
    assert parts_for("pio_spi_flash", "pico2", {flash.replaces}) == ()


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


def test_a_result_with_a_flash_stand_in_carries_its_label():
    flash = EMULATION["pio_spi_flash"]
    item = types.SimpleNamespace(callspec=types.SimpleNamespace(params={"emulation": flash}), user_properties=[])
    platform.pytest_collection_modifyitems([item])
    assert item.user_properties == [("emulator_stand_ins", "pin_spi_flash_gp17")]


def test_gps_uart_takes_the_pin_level_wires_on_gp4_and_gp5():
    # gps_uart.c:20-23: uart1 TX on GP4, RX on GP5; the part sends on the
    # program's RX and listens on its TX.
    assert EMULATION["gps_uart"] == Emulation(i2c0_wiring="wires")
    (gps,) = PARTS["gps_uart"]
    assert gps.bindings["uart"] == (5, 4)


def test_hello_pwm_runs_with_gp0_and_gp1_unwired_and_says_so():
    e = EMULATION["hello_pwm"]
    (unwired,) = e.stand_ins
    assert unwired.pins == (0, 1) and unwired.replaces is None
    assert e.stand_in_label == "stand-in-unwired_gp0_gp1"
    # Fixture 1's crossed UART0 pair reaches both pins on both wires.
    wires = {0: ("uart0_dut_tx", "uart0_harness_tx"), 1: ("uart0_harness_tx", "uart0_dut_tx"), 2: ("gp2",)}
    assert unwired.monitor_commands("pico2", wires) == ("uart0_dut_tx Dispose", "uart0_harness_tx Dispose")
    assert unwired.monitor_commands("pico", {0: ("gp0",), 1: ("gp1",)}) == ("gp0 Dispose", "gp1 Dispose")
    from test.pico_examples.hazards import HARDWARE_HAZARDS
    assert "hello_pwm" in HARDWARE_HAZARDS
