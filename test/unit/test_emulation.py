# SPDX-License-Identifier: Apache-2.0 OR MIT
# What the pico-examples programs need of the emulated bench, the programs
# it cannot judge, and how the emulator platform applies both.

import json
import types
from decimal import Decimal

import pytest

from test.pico_examples import emulation as emu
from test.rig.platforms.emulator.bench import LAYOUTS, ONE_MACHINE_LAYOUTS
from test.pico_examples.emulation import EMULATION, EMULATOR_LIMITS, Emulation, emulation_for
from test.pico_examples.part_targets import PART_SPECS
from test.pico_examples.parts import JUMPERS, PARTS
from test.pico_examples.targets import SPECS
from test.rig.platforms.emulator import platform
from test.rig.platforms.hardware import platform as hardware


def test_every_need_keeps_the_latency_above_two_quanta_in_whole_microseconds():
    for target, e in EMULATION.items():
        quantum = Decimal(e.quantum or "0.0001")
        latency = 300 if e.wire_latency_us is None else e.wire_latency_us
        assert isinstance(latency, int) and not isinstance(latency, bool) and latency > 0, target
        assert Decimal(latency) / 1_000_000 > 2 * quantum, target
        assert e.i2c0_wiring in (None, "bus", "wires"), target


def test_every_harness_relayed_jumper_runs_at_a_fine_quantum_unless_out_of_reach():
    relayed = {t for t, parts in JUMPERS.items() if all(p.model == "jumper" for p in parts)}
    assert relayed - set(EMULATOR_LIMITS)
    for target in relayed - set(EMULATOR_LIMITS):
        e = EMULATION[target]
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


def test_a_program_without_needs_and_a_rig_image_get_the_defaults():
    assert emulation_for({"target": "hello_serial"}) is emu.DEFAULT
    assert emulation_for(None) is emu.DEFAULT


# ---- emulator limits -------------------------------------------------------


def test_the_emulator_limits_name_the_programs_whose_timing_the_wires_cannot_carry():
    assert set(emu.WIRE_LIMITS) == {"slave_mem_i2c", "slave_mem_i2c_burst", "pio_i2c_bus_scan", "pio_clocked_input",
                                    "pio_spi_flash", "spi_slave"}
    for target, reason in emu.WIRE_LIMITS.items():
        assert target in PARTS and target in set(SPECS) | set(PART_SPECS), target
        assert "(300 us; 3 us at best with RELAYED)" in reason, target
        assert reason.endswith("; pending a Renode change that puts the harness's pins in the DUT's time domain")
    assert "open-drain I2C join" in EMULATOR_LIMITS["slave_mem_i2c"]
    assert EMULATOR_LIMITS["slave_mem_i2c_burst"] == EMULATOR_LIMITS["slave_mem_i2c"]
    assert "100 times slower than real time" in EMULATOR_LIMITS["hstx_spi_lcd"]
    assert "21 MHz SPI jumpers" in EMULATOR_LIMITS["pio_clocked_input"]
    assert "samples MISO on its own clock edges, so a harness flash answers a round trip" in (
        EMULATOR_LIMITS["pio_spi_flash"])
    assert "samples the DUT's MISO a round trip" in EMULATOR_LIMITS["spi_slave"]
    assert "reads ff" in EMULATOR_LIMITS["spi_slave"]


# The OTP programs never run on a bench (hazards.py). hello_otp is judged in
# the emulator; the encrypted ones say what the emulated bench lacks.
def test_every_hardware_hazard_is_judged_or_explained_in_the_emulator():
    from test.pico_examples.hazards import HARDWARE_HAZARDS
    from test.pico_examples.pin_specs import REGISTER_SPECS
    assert set(emu.UNPROVISIONED) == {"hello_encrypted", "hello_encrypted_mbedtls", "enc_bootloader",
                                      "hello_serial_enc"}
    assert set(emu.UNPROVISIONED) <= set(HARDWARE_HAZARDS) <= set(EMULATOR_LIMITS) | {"hello_otp"}
    for target, reason in emu.UNPROVISIONED.items():
        assert EMULATOR_LIMITS[target] == reason and "OTP is blank" in reason, target
        assert target not in SPECS and target not in PART_SPECS, target
    assert "hello_otp" not in EMULATOR_LIMITS
    assert SPECS["hello_otp"].source == REGISTER_SPECS["hello_otp"].source == "otp/hello_otp/hello_otp.c"


def test_the_pin_level_i2c_programs_take_the_i2c0_wires_in_one_machine():
    for target in ("slave_mem_i2c", "slave_mem_i2c_burst", "pio_i2c_bus_scan"):
        assert EMULATION[target] == Emulation(i2c0_wiring="wires"), target
        assert target in emu.WIRE_LIMITS and target not in emu.ONE_MACHINE_LIMITS, target
    assert {2, 3} <= {p for part in PARTS["pio_i2c_bus_scan"] for p in part.dut_pins()}
    assert "i2c_bus_scan" not in EMULATOR_LIMITS


def test_a_program_no_emulated_bench_can_judge_needs_nothing_of_it():
    # A program limited only on two machines may need its wiring in one.
    assert not set(emu.ONE_MACHINE_LIMITS) & set(EMULATION)
    for target in emu.ONE_MACHINE_LIMITS:
        assert emulation_for({"target": target}) is emu.DEFAULT, target


class Reached(Exception):
    pass


def limited_image(tmp_path, target, board="pico", chip="rp2040"):
    image = tmp_path / f"{target}.elf"
    image.touch()
    (tmp_path / "example.json").write_text(json.dumps({"target": target, "board": board, "chip": chip}))
    return image


def fixture_function(fixture):
    return getattr(fixture, "_get_wrapped_function", lambda: fixture.__wrapped__)()


CONFIG = types.SimpleNamespace(stash={})


class Untouchable:
    # A harness any use of which fails the test.
    def __getattr__(self, name):
        raise AssertionError(f"the harness was used ({name}) before the skip")


@pytest.mark.parametrize("target", sorted(EMULATOR_LIMITS))
def test_the_emulator_example_fixture_skips_a_limited_program_before_anything_else(tmp_path, monkeypatch, target):
    def reached(*args, **kwargs):
        raise Reached
    monkeypatch.setattr(platform, "Example", reached)
    monkeypatch.setattr(platform, "hold_idle", reached)
    bench = types.SimpleNamespace(dut_image=limited_image(tmp_path, target), layout=LAYOUTS["rpi_pi_pico"],
                                  settings=types.SimpleNamespace(unmet=()))
    with pytest.raises(pytest.skip.Exception) as skipped:
        fixture_function(platform.example)(bench, Untouchable(), None, {}, emu.DEFAULT, CONFIG)
    assert str(skipped.value) == f"{target} is not judged in the emulator: {EMULATOR_LIMITS[target]}"


def test_the_emulator_example_fixture_goes_on_with_a_program_it_can_judge(tmp_path, monkeypatch):
    def reached(*args, **kwargs):
        raise Reached
    monkeypatch.setenv("METALCLAW_ARM_TOOLCHAIN_DIR", str(tmp_path))
    monkeypatch.setattr(platform, "Example", reached)
    bench = types.SimpleNamespace(dut_image=limited_image(tmp_path, "spi_master"), layout=LAYOUTS["rpi_pi_pico"])
    with pytest.raises(Reached):
        fixture_function(platform.example)(bench, Untouchable(), None, {}, emu.DEFAULT, CONFIG)


@pytest.mark.parametrize("target", sorted(emu.WIRE_LIMITS))
def test_a_one_machine_bench_judges_the_programs_the_delayed_wires_cannot(tmp_path, monkeypatch, target):
    def reached(*args, **kwargs):
        raise Reached
    monkeypatch.setenv("METALCLAW_ARM_TOOLCHAIN_DIR", str(tmp_path))
    monkeypatch.setattr(platform, "Example", reached)
    bench = types.SimpleNamespace(dut_image=limited_image(tmp_path, target), layout=ONE_MACHINE_LAYOUTS["rpi_pi_pico"])
    with pytest.raises(Reached):
        fixture_function(platform.example)(bench, Untouchable(), None, {}, emu.DEFAULT, CONFIG)


def test_the_hardware_example_fixture_ignores_the_emulator_limits(tmp_path, monkeypatch):
    def reached(*args, **kwargs):
        raise Reached
    monkeypatch.setenv("METALCLAW_DUT_IMAGE", str(limited_image(tmp_path, "spi_slave")))
    monkeypatch.setenv("METALCLAW_ARM_TOOLCHAIN_DIR", str(tmp_path))
    monkeypatch.setattr(hardware, "Example", reached)
    harness = types.SimpleNamespace(board=lambda: "metalclaw_pico_rp2")
    with pytest.raises(Reached):
        fixture_function(hardware.example)(harness, None, {}, CONFIG)


def test_the_session_emulation_comes_from_the_dut_images_program(tmp_path, monkeypatch):
    monkeypatch.setenv("METALCLAW_DUT_IMAGE", str(limited_image(tmp_path, "gps_uart", "pico2", "rp2350")))
    assert platform.image_emulation() is EMULATION["gps_uart"]


def test_no_dut_image_gives_the_defaults(tmp_path, monkeypatch):
    monkeypatch.setenv("METALCLAW_DUT_IMAGE", str(tmp_path / "missing.elf"))
    assert platform.image_emulation() is emu.DEFAULT


def test_gps_uart_takes_the_pin_level_wires_on_gp4_and_gp5():
    # gps_uart.c:20-23: uart1 TX on GP4, RX on GP5; the part sends on the
    # program's RX and listens on its TX.
    assert EMULATION["gps_uart"] == Emulation(i2c0_wiring="wires")
    (gps,) = PARTS["gps_uart"]
    assert gps.bindings["uart"] == (5, 4)


def test_mpu9250_spi_gets_pin_level_wires_on_gp4_and_gp5():
    # Its MISO and CS are GP4 and GP5 (mpu9250_spi.c:43-46).
    assert EMULATION["mpu9250_spi"] == Emulation(i2c0_wiring="wires")


@pytest.mark.parametrize("target", sorted(emu.ONE_MACHINE_LIMITS))
def test_a_one_machine_bench_still_skips_what_the_wires_do_not_explain(tmp_path, monkeypatch, target):
    monkeypatch.setattr(platform, "Example", lambda *a, **k: (_ for _ in ()).throw(Reached()))
    bench = types.SimpleNamespace(dut_image=limited_image(tmp_path, target), layout=ONE_MACHINE_LAYOUTS["rpi_pi_pico"],
                                  settings=types.SimpleNamespace(unmet=()))
    with pytest.raises(pytest.skip.Exception) as skipped:
        fixture_function(platform.example)(bench, Untouchable(), None, {}, emu.DEFAULT, CONFIG)
    assert str(skipped.value) == f"{target} is not judged in the emulator: {emu.ONE_MACHINE_LIMITS[target]}"
