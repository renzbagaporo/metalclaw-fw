# SPDX-License-Identifier: Apache-2.0 OR MIT
# The DUT's flash as the probe leaves it after a download: every loadable
# segment of the image at its load address. Initialised data loads into
# flash and runs from RAM, so segments are placed by their physical address,
# and one that loads into RAM is not part of the flash.

import struct

FLASH_BASE = 0x10000000
FLASH_BYTES = 0x400000
ERASED = 0xFF

ELF_MAGIC = b"\x7fELF"
ELFCLASS32 = 1
ELFDATA2LSB = 1
PT_LOAD = 1
HEADER = struct.Struct("<16xHHIIIIIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIIIIIII")


class DutFlashError(ValueError):
    pass


def flash_contents(elf: bytes) -> bytes:
    if len(elf) < HEADER.size or elf[:4] != ELF_MAGIC or elf[4] != ELFCLASS32 or elf[5] != ELFDATA2LSB:
        raise DutFlashError("the DUT image is not a little-endian 32-bit ELF")
    _, _, _, _, phoff, _, _, _, phentsize, phnum, _, _, _ = HEADER.unpack_from(elf)
    if phentsize < PROGRAM_HEADER.size or phoff + phnum * phentsize > len(elf):
        raise DutFlashError("the DUT image's program headers are truncated")
    image = bytearray()
    for index in range(phnum):
        kind, offset, _, physical, size, _, _, _ = PROGRAM_HEADER.unpack_from(elf, phoff + index * phentsize)
        if kind != PT_LOAD or size == 0 or not FLASH_BASE <= physical < FLASH_BASE + FLASH_BYTES:
            continue
        start = physical - FLASH_BASE
        end = start + size
        if end > FLASH_BYTES:
            raise DutFlashError(f"the segment at 0x{physical:08x} runs past the end of flash")
        if offset + size > len(elf):
            raise DutFlashError(f"the segment at 0x{physical:08x} is truncated")
        if len(image) < end:
            image.extend(bytes([ERASED]) * (end - len(image)))
        image[start:end] = elf[offset:offset + size]
    if not image:
        raise DutFlashError("the DUT image loads nothing into flash")
    return bytes(image)
