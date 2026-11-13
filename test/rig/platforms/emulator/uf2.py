# SPDX-License-Identifier: Apache-2.0 OR MIT
# A harness's flash from its UF2, as the boot ROM would write it: each main
# flash block of the chip's family at its address, erased bytes between
# them. Blocks of another family are for another target and are left out,
# such as the RP2350 E10 workaround block pico-sdk puts in every RP2350 UF2.

import struct

FLASH_BASE = 0x10000000
# The XIP window; the flash behind it may be smaller.
FLASH_WINDOW_BYTES = 0x1000000
BLOCK_BYTES = 512
PAYLOAD_BYTES = 256
ERASED = 0xFF

MAGIC_START0 = 0x0A324655
MAGIC_START1 = 0x9E5D5157
MAGIC_END = 0x0AB16F30
FLAG_NOT_MAIN_FLASH = 0x00000001
FLAG_FAMILY_ID_PRESENT = 0x00002000
HEADER = struct.Struct("<8I")

# The family IDs pico-sdk writes for each chip's own image.
FAMILY = {
    "rp2040": 0xE48BFF56,
    "rp2350": 0xE48BFF59,  # RP2350 Arm Secure
}


class Uf2Error(ValueError):
    pass


def flash_contents(uf2: bytes, chip: str) -> bytes:
    family = FAMILY[chip]
    if not uf2 or len(uf2) % BLOCK_BYTES:
        raise Uf2Error(f"a UF2 is a whole number of {BLOCK_BYTES}-byte blocks")
    blocks = {}
    for start in range(0, len(uf2), BLOCK_BYTES):
        number = start // BLOCK_BYTES
        magic0, magic1, flags, address, size, _, _, block_family = HEADER.unpack_from(uf2, start)
        if (magic0, magic1, struct.unpack_from("<I", uf2, start + BLOCK_BYTES - 4)[0]) != (
                MAGIC_START0, MAGIC_START1, MAGIC_END):
            raise Uf2Error(f"block {number} is not a UF2 block")
        if flags & FLAG_NOT_MAIN_FLASH or not flags & FLAG_FAMILY_ID_PRESENT or block_family != family:
            continue
        if size != PAYLOAD_BYTES or address % PAYLOAD_BYTES or not FLASH_BASE <= address <= FLASH_BASE + FLASH_WINDOW_BYTES - size:
            raise Uf2Error(f"block {number} at 0x{address:08x} is not a {PAYLOAD_BYTES}-byte block in flash")
        if address in blocks:
            raise Uf2Error(f"two blocks write 0x{address:08x}")
        blocks[address] = uf2[start + 32:start + 32 + size]
    if not blocks:
        raise Uf2Error(f"the UF2 has no {chip} flash blocks")
    image = bytearray([ERASED]) * (max(blocks) + PAYLOAD_BYTES - FLASH_BASE)
    for address, payload in blocks.items():
        image[address - FLASH_BASE:address - FLASH_BASE + PAYLOAD_BYTES] = payload
    return bytes(image)
