# SPDX-License-Identifier: Apache-2.0 OR MIT

import struct

import pytest

from test.rig.platforms.emulator import uf2

RP2040 = 0xE48BFF56
RP2350_ARM_S = 0xE48BFF59
ABSOLUTE = 0xE48BFF57


def block(address, family, payload=None, flags=uf2.FLAG_FAMILY_ID_PRESENT, number=0, total=1):
    payload = bytes([address >> 8 & 0xFF]) * 256 if payload is None else payload
    header = struct.pack("<8I", uf2.MAGIC_START0, uf2.MAGIC_START1, flags, address, len(payload), number, total,
                         family)
    return header + payload + bytes(512 - 32 - len(payload) - 4) + struct.pack("<I", uf2.MAGIC_END)


def test_blocks_land_at_their_flash_offsets_with_erased_bytes_between():
    image = uf2.flash_contents(block(0x10000000, RP2040) + block(0x10000200, RP2040), "rp2040")
    assert image == bytes([0x00]) * 256 + bytes([0xFF]) * 256 + bytes([0x02]) * 256


def test_the_rp2350_e10_block_of_another_family_is_left_out():
    image = uf2.flash_contents(block(0x10FFFF00, ABSOLUTE, bytes(256)) + block(0x10000000, RP2350_ARM_S), "rp2350")
    assert image == bytes([0x00]) * 256


def test_a_block_not_for_main_flash_is_left_out():
    not_flash = block(0x10000100, RP2040, flags=uf2.FLAG_FAMILY_ID_PRESENT | uf2.FLAG_NOT_MAIN_FLASH)
    assert uf2.flash_contents(block(0x10000000, RP2040) + not_flash, "rp2040") == bytes(256)


def test_a_uf2_without_the_chips_blocks_is_refused():
    with pytest.raises(uf2.Uf2Error, match="no rp2350 flash blocks"):
        uf2.flash_contents(block(0x10000000, RP2040), "rp2350")


def test_two_blocks_at_one_address_are_refused():
    with pytest.raises(uf2.Uf2Error, match="two blocks write 0x10000000"):
        uf2.flash_contents(block(0x10000000, RP2040) * 2, "rp2040")


@pytest.mark.parametrize("address", [0x20000000, 0x10000080, 0x11000000])
def test_a_block_outside_flash_or_off_its_boundary_is_refused(address):
    with pytest.raises(uf2.Uf2Error, match="is not a 256-byte block in flash"):
        uf2.flash_contents(block(address, RP2040), "rp2040")


def test_a_truncated_or_corrupt_uf2_is_refused():
    with pytest.raises(uf2.Uf2Error, match="whole number"):
        uf2.flash_contents(block(0x10000000, RP2040)[:-1], "rp2040")
    with pytest.raises(uf2.Uf2Error, match="block 0 is not a UF2 block"):
        uf2.flash_contents(b"\x00" * 512, "rp2040")
