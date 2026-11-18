# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness's SPI target engine (PIO and DMA) against a native mode-0
# controller: a BMP388 on SCK GP18, MOSI GP19, MISO GP16, CS GP22. Its SPI
# reads put a dummy byte between the address and the data (BMP388
# datasheet, 5.3.2).

import pytest

from test.engine.stimulus import SpiController

SCK, MOSI, MISO, CS = 18, 19, 16, 22
CHIP_ID = 0x50
# The emulated PIO samples its pins too coarsely for sub-microsecond edges
# yet (renode-rp2, package 40): at 1 MHz it sees no SCK edge, so the
# silicon rate is expected to fail until that lands; 20 kHz passes.
PIO_TIMING = pytest.mark.xfail(strict=True, reason="the emulated PIO misses sub-microsecond edges (package 40)")
RATES = [20_000, pytest.param(1_000_000, marks=PIO_TIMING)]


@pytest.fixture(scope="module", params=RATES, ids=lambda hz: f"{hz // 1000}kHz")
def spi(request, harness, stimulus):
    harness.run("import metalclaw\nfor i in metalclaw.instances():\n    i.detach()\n"
                f"baro = metalclaw.attach('bmp388', 'baro', spi=({SCK}, {MOSI}, {MISO}, {CS}))\n")
    return SpiController(stimulus, SCK, MOSI, MISO, CS, hz=request.param)


def test_the_target_answers_its_chip_id(spi):
    assert spi.transfer(bytes([0x80, 0x00, 0x00]))[2] == CHIP_ID


def test_a_written_register_reads_back(spi):
    # PWR_CTRL (1Bh): pressure and temperature on, sleep mode.
    spi.transfer(bytes([0x1B, 0x03]))
    assert spi.transfer(bytes([0x9B, 0x00, 0x00]))[2] == 0x03
