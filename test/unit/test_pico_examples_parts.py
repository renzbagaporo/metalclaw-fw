# SPDX-License-Identifier: Apache-2.0 OR MIT

import re
import types
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


def console_case(target, board):
    from test.pico_examples import cases
    example = types.SimpleNamespace(target=target, metadata={"board": board},
                                    restart=lambda: pytest.fail("restarted"))
    cases.test_the_example_completes(None, example, {"line_pullups": True}, None, lambda *a: None)


def test_a_console_with_nothing_to_match_skips_rather_than_passes():
    assert PART_SPECS["lcd_uart"].console == () and not PART_SPECS["lcd_uart"].usb_console
    with pytest.raises(pytest.skip.Exception, match="lcd_uart prints nothing on its console"):
        console_case("lcd_uart", "pico")


def test_a_build_a_console_cannot_judge_skips_with_the_reason():
    from test.pico_examples.part_targets import SUBSTEP_PICO2
    with pytest.raises(pytest.skip.Exception) as skipped:
        console_case("pio_quadrature_encoder_substep", "pico2")
    assert str(skipped.value) == f"pio_quadrature_encoder_substep's console cannot judge its pico2 build: {SUBSTEP_PICO2}"
    with pytest.raises(pytest.skip.Exception, match=r"dht's console cannot judge its pico build: at 125 MHz"):
        console_case("dht", "pico")
    assert PART_SPECS["dht"].for_board("pico2").other_boards == ""


# The encoder drives the DUT's pins and sees nothing of the program: what
# the program prints judges it, and the part has no checks to pass.
def test_the_encoders_are_judged_by_their_programs_counts_alone():
    for target in ("pio_quadrature_encoder", "pio_quadrature_encoder_substep"):
        assert all(not part.checks for part in PARTS[target]), target
        assert PART_SPECS[target].console, target


def test_the_substep_speed_is_what_64_substeps_over_1_ms_give_within_1_us():
    spec = PART_SPECS["pio_quadrature_encoder_substep"].for_board("pico")
    line = "pos: 123        speed: {:<10d}  raw_steps: 2          \n"
    head = "Hello from quadrature encoder substep\n"

    def judged(forward, back):
        return not spec.match(head + line.format(forward) * 3 + line.format(back) * 3)[0]
    # 64 << 20 over 1001, 1000 and 999 us, times 62500 >> 16.
    assert judged(63935, -63936) and judged(63999, -64000) and judged(64064, -64065)
    assert not judged(63934, -64000) and not judged(64065, -64000)
    assert not judged(64000, -63935) and not judged(64000, -64066) and not judged(63000, -63000)


def check_of(target, description_part):
    (part,) = PARTS[target]
    (check,) = [c for c in part.checks if description_part in c.description]
    return check


# mcp9808_i2c.c:70-99: each limit's two bytes at 1/16 degC.
def test_mcp9808_i2cs_limits_are_read_back_from_the_part():
    check = check_of("mcp9808_i2c", "limits")
    limits = [(msb << 8 | lsb) * 1000 // 16 for msb, lsb in ((0x01, 0xE0), (0x01, 0x40), (0x02, 0x80))]
    assert limits == [30000, 20000, 40000]
    assert check.test(limits) and check.test(tuple(limits))
    assert not check.test([0, 0, 0]) and not check.test([30000, 20000, 0])


def test_pcf8523_i2cs_time_is_read_back_from_the_part():
    check = check_of("pcf8523_i2c", "time")
    assert check.after_seconds == 2.0
    assert check.test(((0, 0, 0, 0, 0, 0, 1), False, False)) and check.test(([0, 0, 0, 0, 0, 0, 2], False, False))
    # The time parts.py set, a stopped clock, OS left set, or seconds that
    # did not run.
    assert not check.test(((26, 11, 15, 0, 12, 34, 58), False, False))
    assert not check.test(((0, 0, 0, 0, 0, 0, 1), False, True))
    assert not check.test(((0, 0, 0, 0, 0, 0, 1), True, False))
    assert not check.test(((0, 0, 0, 0, 0, 0, 0), False, False))


def test_lcd_uart_counts_the_bytes_of_the_judged_run_only():
    from test.pico_examples.parts import PARTS
    (lcd,) = PARTS["lcd_uart"]
    count = lcd.checks[0]
    assert count.setup == "lcd_at_restart = part.stats('uart')"
    assert count.before_restart
    assert count.expression == ("(part.stats('uart')['bytes_rx'] - lcd_at_restart['bytes_rx'], "
                                "part.stats('uart')['frame_errors'] - lcd_at_restart['frame_errors'])")
    # One run: 0xFE 0x42, then its fault; a part attached mid-run must not
    # turn an earlier run's lone byte into a failure, or two runs into a pass.
    assert count.test((2, 0)) and not count.test((1, 1)) and not count.test((3, 0)) and not count.test((4, 0))


def test_no_idle_level_holds_a_pin_the_program_drives_with_no_part_on_it():
    from test.pico_examples.parts import PROGRAM_OUTPUTS, parts_dut_pins
    assert PROGRAM_OUTPUTS["adc_dma_capture"] == (6, 7, 8, 9, 10)
    assert {6, 7, 8, 9, 10} <= parts_dut_pins("adc_dma_capture", "pico")
    assert not {6, 7, 8, 9, 10} & parts_dut_pins("blink", "pico")


def test_a_failed_part_check_reports_its_whole_value_and_the_parts_bus_counters():
    from test.pico_examples import cases
    from test.pico_examples.parts import Check

    class Harness:
        def run(self, code):
            if ".stats('spi')" in code:
                return "{'short_windows': 3}\n"
            return repr(bytes(range(256)) * 4) + "\n"

        def sleep(self, seconds):
            pass

    part = Attach("ssd1309", "oled", {"spi": (18, 19, 17, 20), "reset": (21,)},
                  checks=(Check("part.read_region('gddram', 0, 1024)", lambda v: False, "the frame", 1.0),))
    example = types.SimpleNamespace(target="ssd1309_spi", restart=lambda: None)
    recorded = {}
    with pytest.raises(AssertionError):
        cases.test_the_example_parts_saw_the_program(Harness(), example, [part], recorded.__setitem__)
    assert recorded["part_check_0_oled"] == repr(bytes(range(256)) * 4)
    assert recorded["part_oled_spi_stats"] == "{'short_windows': 3}"


def test_a_failed_uart_part_check_reports_the_harness_uarts_registers():
    from test.pico_examples import cases
    from test.pico_examples.parts import Check

    class Harness:
        def run(self, code):
            if code == cases.HARNESS_UARTS:
                return ("uart0 fr=0x90 ris=0x0 mis=0x0 imsc=0x0 dr=empty\n"
                        "uart1 fr=0x80 ris=0x40 mis=0x40 imsc=0x50 dr=0x42\n")
            if code == cases.HARNESS_PIOS:
                return "pio0 fstat=0xf00 fdebug=0x0 flevel=0x1 irq=0x0 addr=[3, 0, 0, 0]\n"
            if ".stats('uart')" in code:
                return "{'bytes_rx': 1}\n"
            return "1\n"

        def sleep(self, seconds):
            pass

    part = Attach("lcd_backpack", "lcd", {"uart": (9, 8)}, checks=(Check("1", lambda v: False, "two bytes", 1.0),))
    example = types.SimpleNamespace(target="lcd_uart", restart=lambda: None)
    recorded = {}
    with pytest.raises(AssertionError):
        cases.test_the_example_parts_saw_the_program(Harness(), example, [part], recorded.__setitem__)
    assert recorded["harness_uart_registers"].endswith("uart1 fr=0x80 ris=0x40 mis=0x40 imsc=0x50 dr=0x42")
    assert recorded["harness_pio_registers"] == "pio0 fstat=0xf00 fdebug=0x0 flevel=0x1 irq=0x0 addr=[3, 0, 0, 0]"
