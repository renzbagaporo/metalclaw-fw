# SPDX-License-Identifier: Apache-2.0 OR MIT
# What the pico-examples programs need of the emulated bench, the programs
# it cannot judge, and how the emulator platform applies both.

import json
import types

import pytest

from test.pico_examples import emulation as emu
from test.rig.platforms.emulator.bench import LAYOUTS
from test.pico_examples.emulation import EMULATION, EMULATOR_LIMITS, Emulation, emulation_for
from test.pico_examples.part_targets import PART_SPECS
from test.pico_examples.parts import JUMPERS, PARTS
from test.pico_examples.targets import SPECS
from test.rig.platforms.emulator import platform
from test.rig.platforms.hardware import platform as hardware


def test_every_need_is_a_known_i2c0_wiring():
    for target, e in EMULATION.items():
        assert e.i2c0_wiring in ("bus", "wires"), target


def test_every_program_on_gp4_or_gp5_off_i2c_takes_the_pin_level_i2c0_wires():
    for target in ("pio_uart_rx", "pio_uart_rx_intr", "pio_uart_dma", "pwm_measure_duty_cycle",
                   "pio_clocked_input", "gps_uart", "mpu9250_spi"):
        assert EMULATION[target] is emu.GP4_GP5_WIRES, target
    for target in ("pio_uart_rx", "pio_uart_rx_intr", "pio_uart_dma", "pwm_measure_duty_cycle",
                   "pio_clocked_input"):
        assert {4, 5} & {p for part in PARTS[target] for p in part.dut_pins()}, target


def test_a_relayed_jumper_off_gp4_and_gp5_needs_nothing_of_the_bench():
    relayed = {t for t, parts in JUMPERS.items() if all(p.model == "jumper" for p in parts)}
    for target in relayed - set(EMULATOR_LIMITS):
        if not {4, 5} & {p for part in PARTS[target] for p in part.dut_pins()}:
            assert emulation_for({"target": target}) is emu.DEFAULT, target


def test_a_program_without_needs_and_a_rig_image_get_the_defaults():
    assert emulation_for({"target": "hello_serial"}) is emu.DEFAULT
    assert emulation_for({"target": "pio_onewire"}) is emu.DEFAULT
    assert emulation_for(None) is emu.DEFAULT


# ---- emulator limits -------------------------------------------------------


def test_the_emulator_limits_are_the_unprovisioned_the_too_slow_the_core_order_and_the_races():
    assert set(EMULATOR_LIMITS) == (set(emu.UNPROVISIONED) | set(emu.SPEED_LIMITS) | set(emu.CORE_ORDER_LIMITS)
                                    | set(emu.RACE_LIMITS))
    assert set(emu.SPEED_LIMITS) == {"hstx_spi_lcd", "hello_pwm", "pio_hub75"}
    for target in emu.SPEED_LIMITS:
        assert "times slower than real time" in EMULATOR_LIMITS[target], target
    assert set(emu.CORE_ORDER_LIMITS) == {"multicore_doorbell"}
    assert set(emu.RACE_LIMITS) == {"lcd_uart"}


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


def test_the_pin_level_i2c_programs_take_the_i2c0_wires():
    for target in ("slave_mem_i2c", "slave_mem_i2c_burst", "pio_i2c_bus_scan"):
        assert EMULATION[target] is emu.I2C_PIN_WIRES, target
        assert target not in EMULATOR_LIMITS, target
    assert {2, 3} <= {p for part in PARTS["pio_i2c_bus_scan"] for p in part.dut_pins()}


def test_a_program_the_emulator_cannot_judge_needs_nothing_of_it():
    assert not set(EMULATOR_LIMITS) & set(EMULATION)


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


def start_bench(tmp_path, monkeypatch, target):
    # The bench fixture up to Bench.start, which a skip must come before.
    def reached(*args, **kwargs):
        raise Reached
    monkeypatch.setenv("METALCLAW_DUT_IMAGE", str(limited_image(tmp_path, target)))
    monkeypatch.setenv("METALCLAW_HARNESS_BOARD", "rpi_pi_pico")
    monkeypatch.setenv("METALCLAW_RENODE_DIR", str(tmp_path))
    monkeypatch.setenv("METALCLAW_RENODE_ROMS", str(tmp_path))
    monkeypatch.setattr(platform, "Bench", reached)
    monkeypatch.setattr(platform, "soc_series", lambda image: platform.layout().dut_chip)
    for name in platform.SLOW_HOST_DEFAULTS:
        monkeypatch.delenv(name, raising=False)
    next(fixture_function(platform.bench)(emu.DEFAULT, CONFIG, lambda *a: None))


@pytest.mark.parametrize("target", sorted(EMULATOR_LIMITS))
def test_the_bench_skips_a_limited_program_before_renode_starts(tmp_path, monkeypatch, target):
    with pytest.raises(pytest.skip.Exception) as skipped:
        start_bench(tmp_path, monkeypatch, target)
    assert str(skipped.value) == f"{target} is not judged in the emulator: {EMULATOR_LIMITS[target]}"


def test_the_bench_starts_for_a_program_it_can_judge(tmp_path, monkeypatch):
    with pytest.raises(Reached):
        start_bench(tmp_path, monkeypatch, "spi_master")


def test_the_emulator_example_fixture_goes_on_with_a_program_it_can_judge(tmp_path, monkeypatch):
    def reached(*args, **kwargs):
        raise Reached
    monkeypatch.setenv("METALCLAW_ARM_TOOLCHAIN_DIR", str(tmp_path))
    monkeypatch.setattr(platform, "Example", reached)
    bench = types.SimpleNamespace(dut_image=limited_image(tmp_path, "spi_master"), layout=LAYOUTS["rpi_pi_pico"])
    with pytest.raises(Reached):
        fixture_function(platform.example)(bench, Untouchable(), None, {}, emu.DEFAULT, CONFIG)


def test_the_hardware_example_fixture_ignores_the_emulator_limits(tmp_path, monkeypatch):
    def reached(*args, **kwargs):
        raise Reached
    monkeypatch.setenv("METALCLAW_DUT_IMAGE", str(limited_image(tmp_path, "hello_pwm")))
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
