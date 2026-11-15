# SPDX-License-Identifier: Apache-2.0 OR MIT
# Register criteria for programs whose pin edges are too fast to watch:
# words read over SWD after a restart, masked, against what each program's
# source writes, at the pico-sdk 2.3.0 hardware_regs addresses.

import contextlib
import types

import pytest

from test.pico_examples import cases
from test.pico_examples.hazards import HARDWARE_HAZARDS
from test.pico_examples.pin_specs import REGISTER_SPECS, Register


def test_a_register_is_judged_under_its_mask():
    r = Register(0x40050010, 0xFFFF, 3, "TOP")
    assert r.check(0xABCD0003) is None
    assert r.check(0x4) == "TOP: 0x40050010 & 0xffff is 0x4, expected 0x3"


def test_a_register_may_hold_any_of_several_values():
    r = Register(0x502000D8, 0xFFFF, (0xE101, 0xE000, 0x0001), "SM0_INSTR")
    assert all(r.check(v) is None for v in (0xE101, 0xE000, 0x10001))
    assert r.check(0xE081) == "SM0_INSTR: 0x502000d8 & 0xffff is 0xe081, expected 0xe101 or 0xe000 or 0x1"


def test_every_register_spec_covers_both_chips_with_aligned_reachable_words():
    for target, spec in REGISTER_SPECS.items():
        assert spec.source, target
        assert set(spec.registers) == {"rp2040", "rp2350"}, target
        for chip, registers in spec.registers.items():
            assert registers, (target, chip)
            for r in registers:
                assert r.address % 4 == 0, (target, chip, r)
                for value in (r.expected if isinstance(r.expected, tuple) else (r.expected,)):
                    assert value & ~r.mask == 0, (target, chip, r)


def by_what(target, chip):
    return {r.what: r for r in REGISTER_SPECS[target].registers[chip]}


def test_hello_pwm_sets_slice_0_as_its_source_does():
    for chip, pwm, io in (("rp2040", 0x40050000, 0x40014000), ("rp2350", 0x400A8000, 0x40028000)):
        r = by_what("hello_pwm", chip)
        assert (r["PWM CH0_CSR.EN"].address, r["PWM CH0_TOP"].address) == (pwm, pwm + 0x10)
        assert r["PWM CH0_CC B 3, A 1"].expected == 0x00030001
        assert (r["GP0 FUNCSEL PWM"].address, r["GP1 FUNCSEL PWM"].address) == (io + 0x04, io + 0x0C)
    # GP1 is the DUT's console RX: the bench never runs it.
    assert "hello_pwm" in HARDWARE_HAZARDS


def test_pio_squarewave_runs_its_loop_at_two_and_a_half():
    r = by_what("pio_squarewave", "rp2040")
    assert r["PIO0 SM0_CLKDIV 2.5"].expected == int(2.5 * (1 << 16))
    assert r["PIO0 SM0_PINCTRL SET_COUNT 1, SET_BASE 0"].expected == 0x04000000
    assert r["GP0 FUNCSEL PIO0"].expected == 6


def test_hello_gpout_divides_each_source_by_10_in_each_chips_layout():
    r40, r50 = by_what("hello_gpout", "rp2040"), by_what("hello_gpout", "rp2350")
    assert [r40[f"CLK_GPOUT{n}_CTRL ENABLE, AUXSRC"].expected for n in range(4)] == [0x8C0, 0x8E0, 0x900, 0x920]
    assert [r50[f"CLK_GPOUT{n}_CTRL ENABLE, AUXSRC"].expected for n in range(4)] == [0x900, 0x920, 0x940, 0x980]
    assert r40["CLK_GPOUT3_DIV 10"].address == 0x40008028 and r40["CLK_GPOUT3_DIV 10"].expected == 0xA00
    assert r50["CLK_GPOUT3_DIV 10"].address == 0x40010028 and r50["CLK_GPOUT3_DIV 10"].expected == 0xA0000
    assert r50["GP25 FUNCSEL GPOUT3"].address == 0x400280CC and r50["GP25 FUNCSEL GPOUT3"].expected == 9


class Memory:
    def __init__(self, words):
        self.words = words
        self.read = []

    @contextlib.contextmanager
    def link(self):
        yield types.SimpleNamespace(read_words=lambda address, count: self.read.append(address)
                                    or [self.words.get(address, 0)])


def run_case(target, chip, words):
    harness = types.SimpleNamespace(slept=[], sleep=lambda s: harness.slept.append(s))
    restarts = []
    example = types.SimpleNamespace(target=target, metadata={"chip": chip}, memory=Memory(words),
                                    restart=lambda: restarts.append(True))
    cases.test_the_example_sets_its_registers(harness, example, ())
    return harness, example, restarts


def test_the_case_reads_every_register_after_the_restart():
    spec = REGISTER_SPECS["hello_pwm"]
    words = {r.address: r.expected for r in spec.registers["rp2350"]}
    harness, example, restarts = run_case("hello_pwm", "rp2350", words)
    assert restarts == [True] and harness.slept == [spec.after]
    assert example.memory.read == [r.address for r in spec.registers["rp2350"]]


def test_the_case_names_each_register_that_is_off():
    spec = REGISTER_SPECS["pio_squarewave"]
    words = {r.address: (r.expected[0] if isinstance(r.expected, tuple) else r.expected)
             for r in spec.registers["rp2040"]}
    words[0x50200000] = 0
    with pytest.raises(AssertionError, match=r"PIO0 CTRL.SM_ENABLE SM0: 0x50200000 & 0x1 is 0x0"):
        run_case("pio_squarewave", "rp2040", words)


def test_a_program_without_register_criteria_skips():
    with pytest.raises(pytest.skip.Exception, match="no register criteria for blink"):
        run_case("blink", "rp2040", {})
