# SPDX-License-Identifier: Apache-2.0 OR MIT

import struct

from test.rig.platforms.emulator import dut_flash
from test.rig.platforms.hardware import platform


def elf_with(segments, entry):
    # A minimal little-endian ELF32 with one PT_LOAD per (address, data).
    phoff = 52
    header = bytearray(b"\x7fELF\x01\x01\x01" + bytes(9))
    header += struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, entry, phoff, 0, 0, 52, 32, len(segments), 0, 0, 0)
    body = bytearray()
    offset = phoff + 32 * len(segments)
    headers = bytearray()
    for address, data in segments:
        headers += struct.pack("<IIIIIIII", 1, offset + len(body), address, address, len(data), len(data), 7, 4)
        body += data
    return bytes(header + headers + body)


class FakeLink:
    def __init__(self):
        self.memory = {}
        self.registers = {}
        self.halted = False

    def write_words(self, address, words):
        for i, w in enumerate(words):
            self.memory[address + 4 * i] = w
        if address == platform.AIRCR:
            self.halted = True
        if address == platform.DCRSR:
            self.registers[words[0] & 0x7F] = self.memory[platform.DCRDR]

    def read_words(self, address, count):
        if address == platform.DHCSR:
            return [(platform.DHCSR_S_HALT if self.halted else 0) | platform.DHCSR_S_REGRDY]
        return [self.memory.get(address + 4 * i, 0) for i in range(count)]


def test_a_ram_only_image_loads_nothing_into_flash():
    elf = elf_with([(0x20000000, struct.pack("<II", 0x20042000, 0x200000C1))], 0x200000C1)
    assert not any(dut_flash.in_flash(p) for p, _, _ in dut_flash.segments(elf))
    assert dut_flash.flash_contents(elf, allow_empty=True) == dut_flash.ERASED_FLASH


def test_it_starts_at_its_entry_with_vtor_and_sp_from_its_vectors():
    elf = elf_with([(0x20000000, struct.pack("<II", 0x20042000, 0x200000C1) + b"\xAA\xBB")], 0x200000C1)
    link = FakeLink()
    platform.start_from_ram(link, elf, 0x20000000)
    assert link.memory[0x20000000] == 0x20042000
    assert link.memory[0x20000008] == 0xBBAA
    assert link.memory[platform.VTOR] == 0x20000000
    assert link.registers == {platform.REG_SP: 0x20042000, platform.REG_PC: 0x200000C0,
                              platform.REG_XPSR: platform.XPSR_T}
    assert link.memory[platform.DEMCR] == 0
    assert link.memory[platform.DHCSR] == platform.DHCSR_RESUME
