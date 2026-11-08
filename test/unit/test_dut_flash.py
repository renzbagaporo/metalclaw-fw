# SPDX-License-Identifier: Apache-2.0 OR MIT

import struct

import pytest

from test.rig.platforms.emulator import dut_flash

FLASH = dut_flash.FLASH_BASE
RAM = 0x20000000
PT_NOTE = 4


# segments: (type, physical, virtual, contents, memory size)
def elf(segments, phentsize=32):
    phoff = 52
    data_offset = phoff + len(segments) * phentsize
    headers = bytearray()
    payload = bytearray()
    for kind, physical, virtual, contents, memsz in segments:
        header = struct.pack("<8I", kind, data_offset + len(payload), virtual, physical, len(contents), memsz, 5, 4)
        headers += header.ljust(phentsize, b"\0")
        payload += contents
    ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    header = ident + struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, FLASH | 1, phoff, 0, 0, 52, phentsize, len(segments),
                                 40, 0, 0)
    return header + bytes(headers) + bytes(payload)


def test_a_code_segment_lands_at_its_offset_in_flash():
    assert dut_flash.flash_contents(elf([(dut_flash.PT_LOAD, FLASH, FLASH, b"code", 4)])) == b"code"


def test_data_lands_at_its_load_address_not_its_run_address():
    image = dut_flash.flash_contents(elf([
        (dut_flash.PT_LOAD, FLASH, FLASH, b"code", 4),
        (dut_flash.PT_LOAD, FLASH + 0x100, RAM, b"data", 4),
    ]))
    assert len(image) == 0x104
    assert image[0x100:] == b"data"


def test_the_gap_between_segments_is_erased():
    image = dut_flash.flash_contents(elf([
        (dut_flash.PT_LOAD, FLASH + 0x10, FLASH + 0x10, b"late", 4),
        (dut_flash.PT_LOAD, FLASH, FLASH, b"head", 4),
    ]))
    assert image[:4] == b"head"
    assert set(image[4:0x10]) == {dut_flash.ERASED}
    assert image[0x10:] == b"late"


def test_segments_loaded_into_ram_are_not_in_flash():
    image = dut_flash.flash_contents(elf([
        (dut_flash.PT_LOAD, FLASH, FLASH, b"code", 4),
        (dut_flash.PT_LOAD, RAM, RAM, b"ramfunc", 7),
        (dut_flash.PT_LOAD, RAM + 0x100, RAM + 0x100, b"", 0x400),
    ]))
    assert image == b"code"


def test_only_loadable_segments_are_in_flash():
    image = dut_flash.flash_contents(elf([
        (PT_NOTE, FLASH + 0x100, FLASH + 0x100, b"note", 4),
        (dut_flash.PT_LOAD, FLASH, FLASH, b"code", 4),
    ]))
    assert image == b"code"


def test_a_wider_program_header_entry_is_followed():
    image = dut_flash.flash_contents(elf([
        (dut_flash.PT_LOAD, FLASH, FLASH, b"code", 4),
        (dut_flash.PT_LOAD, FLASH + 4, RAM, b"data", 4),
    ], phentsize=40))
    assert image == b"codedata"


def test_a_file_that_is_not_an_elf_is_refused():
    with pytest.raises(dut_flash.DutFlashError, match="not a little-endian 32-bit ELF"):
        dut_flash.flash_contents(b"\x7fELG" + bytes(60))


def test_a_64_bit_elf_is_refused():
    image = bytearray(elf([(dut_flash.PT_LOAD, FLASH, FLASH, b"code", 4)]))
    image[4] = 2
    with pytest.raises(dut_flash.DutFlashError, match="not a little-endian 32-bit ELF"):
        dut_flash.flash_contents(bytes(image))


def test_an_image_with_nothing_for_flash_is_refused():
    with pytest.raises(dut_flash.DutFlashError, match="loads nothing into flash"):
        dut_flash.flash_contents(elf([(dut_flash.PT_LOAD, RAM, RAM, b"data", 4)]))


def test_a_segment_running_past_the_end_of_flash_is_refused():
    end = FLASH + dut_flash.FLASH_BYTES
    with pytest.raises(dut_flash.DutFlashError, match="runs past the end of flash"):
        dut_flash.flash_contents(elf([(dut_flash.PT_LOAD, end - 2, end - 2, b"code", 4)]))


def test_a_truncated_segment_is_refused():
    with pytest.raises(dut_flash.DutFlashError, match="is truncated"):
        dut_flash.flash_contents(elf([(dut_flash.PT_LOAD, FLASH, FLASH, b"code", 4)])[:-2])


def test_truncated_program_headers_are_refused():
    with pytest.raises(dut_flash.DutFlashError, match="program headers are truncated"):
        dut_flash.flash_contents(elf([(dut_flash.PT_LOAD, FLASH, FLASH, b"code", 4)])[:60])
