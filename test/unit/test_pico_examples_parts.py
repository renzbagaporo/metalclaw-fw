# SPDX-License-Identifier: Apache-2.0 OR MIT

import re
from pathlib import Path

import pytest

from test.pico_examples.part_targets import PART_SPECS
from test.pico_examples.parts import PARTS, Attach, PinError, harness_pins, i2c_pins, parts_for
from test.pico_examples.targets import SPECS
from test.rig.platforms.hardware.platform import FIXTURE, METALCLAW_PICO_RP2_FIXTURE


def test_fixture_1_is_pin_for_pin_but_the_crossed_uart():
    assert harness_pins(FIXTURE, (4, 5)) == (4, 5)
    assert harness_pins(FIXTURE, (0, 1)) == (1, 0)


def test_the_probes_pins_reach_nothing_on_fixture_1():
    with pytest.raises(PinError):
        harness_pins(FIXTURE, (2,))


def test_a_carrier_binding_takes_branches_from_one_pio_window():
    # SPI0 defaults: GP16 RX, GP17 CSn, GP18 SCK, GP19 TX. GP18 has only
    # GPIO38, so all four come from the 16-47 window.
    got = harness_pins(METALCLAW_PICO_RP2_FIXTURE, (18, 19, 16, 17))
    assert got == (38, 32, 35, 34)
    assert all(16 <= p < 48 for p in got)


def test_the_carrier_i2c_default_pair_is_the_rigs_own():
    assert harness_pins(METALCLAW_PICO_RP2_FIXTURE, (4, 5), i2c_pins) == (12, 13)


def test_an_attach_line_carries_harness_pins_then_values():
    a = Attach("bmp280", "baro", {"i2c": (4, 5, 0x76)}, {"i2c": 2}, {"pressure": 90000})
    assert a.code(FIXTURE) == ("import metalclaw\nbaro = metalclaw.attach('bmp280', 'baro', i2c=(4, 5, 118))\n"
                               "baro.pressure = 90000\n")


def test_a_carrier_i2c_attach_takes_the_branches_with_i2c_functions():
    a = Attach("bmp280", "baro", {"i2c": (4, 5, 0x76)}, {"i2c": 2})
    assert "i2c=(12, 13, 118)" in a.code(METALCLAW_PICO_RP2_FIXTURE)


def test_a_pair_without_i2c_functions_is_refused():
    with pytest.raises(PinError):
        harness_pins(FIXTURE, (5, 6), i2c_pins)


# Fixture 1 reaches neither the DUT's GP2 nor its GP3 (nor GP28): a part on
# one of them attaches on the carrier only.
def test_every_part_attaches_on_both_fixtures_but_those_on_pins_fixture_1_lacks():
    for target, parts in PARTS.items():
        for part in parts:
            fixtures = (METALCLAW_PICO_RP2_FIXTURE,) if {2, 3, 28} & set(part.dut_pins()) else (
                FIXTURE, METALCLAW_PICO_RP2_FIXTURE)
            for fixture in fixtures:
                assert f"metalclaw.attach({part.model!r}, {part.name!r}, " in part.code(fixture), target
            if len(fixtures) == 1:
                with pytest.raises(PinError):
                    part.code(FIXTURE)


def test_the_wire_parts_bind_what_the_programs_drive():
    on_fixture_1 = {t for t, parts in PARTS.items() if all(not {2, 3, 28} & set(part.dut_pins()) for part in parts)}
    batch = {"pio_ws2812", "pio_ws2812_parallel", "pio_apa102", "pio_onewire", "dht", "pio_quadrature_encoder",
             "pio_quadrature_encoder_substep", "pio_ir_loopback", "pio_hub75"}
    assert batch <= set(PARTS)
    assert batch & on_fixture_1 == {"pio_onewire", "dht", "pio_quadrature_encoder", "pio_quadrature_encoder_substep",
                                    "pio_ir_loopback"}
    assert {t for t in batch if any(p.needs_line_pullups for p in PARTS[t])} == {"pio_onewire", "dht"}


def test_the_encoder_takes_consecutive_pins_on_both_benches():
    # DUT GP11 has carrier branches 16 and 23; only 23 follows GP10's 22.
    (encoder,) = PARTS["pio_quadrature_encoder"]
    assert "ab=(22, 23)" in encoder.code(METALCLAW_PICO_RP2_FIXTURE)
    assert "ab=(10, 11)" in encoder.code(FIXTURE)


def test_the_hub75_panel_takes_one_pio_window_on_the_carrier():
    (panel,) = PARTS["pio_hub75"]
    code = panel.code(METALCLAW_PICO_RP2_FIXTURE)
    pins = [int(p) for p in re.findall(r"\b(\d+)\b", code.split("attach(", 1)[1])]
    assert pins and all(p < 32 for p in pins), code


def test_the_spi_part_takes_the_carrier_window_branches():
    (env,) = PARTS["bme280_spi"]
    assert "spi=(38, 32, 35, 34)" in env.code(METALCLAW_PICO_RP2_FIXTURE)
    assert "spi=(18, 19, 16, 17)" in env.code(FIXTURE)


# A program that prints nothing is judged by its parts' checks alone.
def test_every_attached_model_is_a_device_and_every_target_has_criteria():
    root = Path(__file__).resolve().parents[2]
    models = set()
    for cmake in (root / "devices").glob("*/device.cmake"):
        models |= set(re.findall(r"metalclaw_device\((\w+)", cmake.read_text()))
    for target, parts in PARTS.items():
        assert {p.model for p in parts} <= models, target
        spec = SPECS.get(target) or PART_SPECS.get(target)
        assert (spec and spec.source) or any(p.checks for p in parts), target


def test_a_part_carries_its_checks():
    from test.pico_examples.parts import Check
    c = Check("part.frames", lambda v: v > 0, "the program drew a frame")
    a = Attach("ssd1306", "oled", {"i2c": (4, 5, 0x3C)}, {"i2c": 2}, checks=(c,))
    assert a.checks[0].test(3) and not a.checks[0].test(0)


# On a bench where a DUT pin reaches two harness pins (the carrier's
# two-branch nets), two harness drivers on one net would short two harness
# outputs. In a program's session the rig takes one harness pin per net:
# its parts' bindings, the console bridge where a case reads the console,
# and the probe's SWD pins.
def session_harness_pins(target, fixture):
    from test.pico_examples.part_targets import PART_SPECS
    from test.pico_examples.pin_specs import PIN_SPECS
    from test.pico_examples.targets import SPECS
    pins = [p for part in parts_for(target, "pico") for p in sum(part._pins(fixture).values(), ())]
    if any(target in specs for specs in (SPECS, PART_SPECS, PIN_SPECS)):
        pins += [fixture["uart"]["tx"], fixture["uart"]["rx"]]
    return pins + list(fixture["swd"])


def test_no_session_puts_two_harness_pins_on_one_dut_net():
    for fixture in (FIXTURE, METALCLAW_PICO_RP2_FIXTURE):
        net = {h: d for d, branches in fixture["dut_pins"].items() for h in branches}
        for target in PARTS:
            try:
                pins = session_harness_pins(target, fixture)
            except PinError:
                continue
            by_net = {}
            for pin in pins:
                if pin in net:
                    by_net.setdefault(net[pin], set()).add(pin)
            assert all(len(p) == 1 for p in by_net.values()), (target, {n: p for n, p in by_net.items() if len(p) > 1})


def test_the_duty_cycle_jumper_drives_one_branch_of_gp5_on_the_carrier():
    (jumper,) = PARTS["pwm_measure_duty_cycle"]
    pins = jumper._pins(METALCLAW_PICO_RP2_FIXTURE)
    assert pins == {"source": (6,), "sink": (10,)}
    assert METALCLAW_PICO_RP2_FIXTURE["dut_pins"][5] == (10, 13)


# i2c/bus_scan/bus_scan.c:56-71 and pio/i2c/i2c_bus_scan.c:25-40: "@" for
# an address that answered, "." for one that did not or is reserved.
def scan(title, answering):
    rows = "".join(f"{r}0 " + "  ".join("@" if 16 * r + c in answering else "." for c in range(16)) + "\n"
                   for r in range(8))
    return f"\n{title}\n   0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n{rows}Done.\n"


@pytest.mark.parametrize("target, title", (("i2c_bus_scan", "I2C Bus Scan"), ("pio_i2c_bus_scan", "PIO I2C Bus Scan")))
def test_a_bus_scan_finds_its_part_and_nothing_else(target, title):
    (part,) = PARTS[target]
    address = part.bindings["i2c"][2]
    spec = SPECS[target]
    assert spec.match(scan(title, {address})) == ([], [])
    assert spec.match(scan(title, set()))[0]
    assert spec.match(scan(title, {address, 0x76}))[0]
    assert spec.match(scan(title, {address + 1}))[0]


def test_the_pio_bus_scan_part_takes_i2c_pins_on_the_carrier_only():
    (part,) = PARTS["pio_i2c_bus_scan"]
    assert "i2c=(6, 7, 24)" in part.code(METALCLAW_PICO_RP2_FIXTURE)
    with pytest.raises(PinError):
        part.code(FIXTURE)
    (part,) = PARTS["i2c_bus_scan"]
    assert "i2c=(4, 5, 24)" in part.code(FIXTURE) and "i2c=(12, 13, 24)" in part.code(METALCLAW_PICO_RP2_FIXTURE)
