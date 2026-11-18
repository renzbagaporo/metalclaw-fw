# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness's I2C target engine against a native controller: a BMP388 on
# the harness's GP4 (SDA) and GP5 (SCL) answers bit-level transfers.

import pytest

from test.engine.stimulus import I2cController

SDA, SCL, ADDRESS = 4, 5, 0x76
CHIP_ID = 0x50


@pytest.fixture(scope="module")
def bus(harness, stimulus):
    harness.run("import metalclaw\nfor i in metalclaw.instances():\n    i.detach()\n"
                f"baro = metalclaw.attach('bmp388', 'baro', i2c=({SDA}, {SCL}, {ADDRESS:#x}))\n")
    return I2cController(stimulus, SDA, SCL)


def test_the_target_answers_its_chip_id(bus):
    result = bus.transfer(ADDRESS, write=b"\x00", read=1)
    assert result.acked and result.data == bytes([CHIP_ID]), result


def test_another_address_is_not_acknowledged(bus):
    assert not bus.transfer(ADDRESS ^ 1, write=b"\x00").acked


def test_a_written_register_reads_back(bus, harness):
    # PWR_CTRL (1Bh): press and temperature on, sleep mode.
    assert bus.transfer(ADDRESS, write=b"\x1b\x03").acked
    assert bus.transfer(ADDRESS, write=b"\x1b", read=1).data == b"\x03"
