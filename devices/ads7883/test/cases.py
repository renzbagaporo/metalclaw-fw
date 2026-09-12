# SPDX-License-Identifier: Apache-2.0 OR MIT

import pytest

NAME = "adc"
SCK = 18
MOSI = 19
MISO = 16
CS = 21

SAMPLES = [0, 1, 0x800, 0xABC, 0xFFF]
CLOCKS_HZ = [1_000_000, 5_000_000, 10_000_000, 15_000_000]
WINDOWS_PER_CLOCK = 1000
# The engine needs about 10 us after chip-select release before the next
# window may start; the DUT's own turnaround is about 5 us.
GAP_US = 20

ATTACH = f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
metalclaw.attach('ads7883', {NAME!r}, spi=({SCK}, {MOSI}, {MISO}, {CS}))
"""

SELECT = f"""import metalclaw
adc = [i for i in metalclaw.instances() if i.name == {NAME!r}][0]
"""


class Converter:
    def __init__(self, harness):
        self.harness = harness

    def set(self, sample):
        self.harness.run(SELECT + f"adc.sample = {sample}\n")

    def get(self, name):
        return int(self.harness.run(SELECT + f"print(adc.{name})\n").strip())

    def stats(self):
        return eval(self.harness.run(SELECT + "print(adc.stats('spi'))\n").strip())


@pytest.fixture(scope="module")
def converter(harness, dut):
    dut.require_image('ads7883')
    harness.run(ATTACH)
    dut.reset()
    dut.ping()
    return Converter(harness)


def read_frame(dut, hz=0, count=1, length=2, gap_us=GAP_US):
    result = dut.command(f"ADC {hz} {length} {count} {gap_us}")
    assert result["test"] == "test_adc", result
    assert result["status"] == "ok", result
    return result


@pytest.mark.parametrize("sample", SAMPLES)
def test_the_dut_reads_what_was_injected(dut, converter, sample):
    converter.set(sample)
    result = read_frame(dut)
    assert result["sample"] == sample, result
    assert result["leading"] == 0 and result["trailing"] == 0, result


def test_a_new_sample_reaches_the_next_window(dut, converter):
    converter.set(0x123)
    assert read_frame(dut)["sample"] == 0x123
    converter.set(0x456)
    assert read_frame(dut)["sample"] == 0x456


def test_the_part_holds_sdo_low_after_the_frame(dut, converter):
    converter.set(0xFFF)
    result = read_frame(dut, length=6)
    assert result["frame"] == "3ffc00000000", result


def test_every_window_moves_bus_traffic(dut, converter):
    before = converter.stats()["transactions"]
    read_frame(dut, count=10)
    assert converter.stats()["transactions"] >= before + 10


@pytest.mark.parametrize("hz", CLOCKS_HZ)
def test_the_frame_is_exact_across_the_clock_range(dut, converter, hz):
    converter.set(0xA5A)
    result = read_frame(dut, hz=hz, count=WINDOWS_PER_CLOCK)
    assert result["sample"] == 0xA5A, result
    assert result["mismatches"] == 0, result
    stats = converter.stats()
    assert stats["stale_windows"] == 0 and stats["dma_faults"] == 0, stats


def test_a_reset_restores_the_default_sample(dut, converter):
    converter.set(0x321)
    converter.harness.run(SELECT + "adc.reset()\n")
    assert converter.get("sample") == 0
    assert read_frame(dut)["sample"] == 0
