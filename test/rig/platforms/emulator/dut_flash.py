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


def segments(elf: bytes) -> list[tuple[int, int, int]]:
    # (physical address, file offset, size) of each loadable segment.
    if len(elf) < HEADER.size or elf[:4] != ELF_MAGIC or elf[4] != ELFCLASS32 or elf[5] != ELFDATA2LSB:
        raise DutFlashError("the DUT image is not a little-endian 32-bit ELF")
    _, _, _, _, phoff, _, _, _, phentsize, phnum, _, _, _ = HEADER.unpack_from(elf)
    if phentsize < PROGRAM_HEADER.size or phoff + phnum * phentsize > len(elf):
        raise DutFlashError("the DUT image's program headers are truncated")
    found = []
    for index in range(phnum):
        kind, offset, _, physical, size, _, _, _ = PROGRAM_HEADER.unpack_from(elf, phoff + index * phentsize)
        if kind == PT_LOAD and size:
            found.append((physical, offset, size))
    return found


def in_flash(address: int) -> bool:
    return FLASH_BASE <= address < FLASH_BASE + FLASH_BYTES


def entry(elf: bytes) -> int:
    segments(elf)
    return HEADER.unpack_from(elf)[3]


# Every loadable segment that lands in flash, as (address, bytes).
def flash_segments(elf: bytes) -> list[tuple[int, bytes]]:
    found = []
    for physical, offset, size in segments(elf):
        if not in_flash(physical):
            continue
        if physical - FLASH_BASE + size > FLASH_BYTES:
            raise DutFlashError(f"the segment at 0x{physical:08x} runs past the end of flash")
        if offset + size > len(elf):
            raise DutFlashError(f"the segment at 0x{physical:08x} is truncated")
        found.append((physical, elf[offset:offset + size]))
    return found


# An image that loads nothing into flash (a no_flash build) runs from SRAM:
# the probe writes it there and starts it. Its flash stays erased.
ERASED_FLASH = bytes([ERASED]) * 4096


def flash_contents(elf: bytes, allow_empty: bool = False) -> bytes:
    image = bytearray()
    for physical, data in flash_segments(elf):
        start = physical - FLASH_BASE
        end = start + len(data)
        if len(image) < end:
            image.extend(bytes([ERASED]) * (end - len(image)))
        image[start:end] = data
    if not image:
        if allow_empty:
            return ERASED_FLASH
        raise DutFlashError("the DUT image loads nothing into flash")
    return bytes(image)
