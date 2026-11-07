# SPDX-License-Identifier: Apache-2.0 OR MIT
# A harness flash that already holds its filesystem, as a bench harness's
# does after its first boot. On fresh flash the harness formats it while USB
# is coming up, with interrupts off for each sector erase; at emulated speed
# that outlasts the host's control-transfer timeouts.

import math
import struct

XIP_BASE = 0x10000000
SECTOR_BYTES = 4096
ERASED = 0xFF

BINARY_INFO_MARKER_START = 0x7188EBF2
BINARY_INFO_MARKER_END = 0xE71AA390
BINARY_INFO_TYPE_BLOCK_DEVICE = 7
BINARY_INFO_TAG_MICROPYTHON = ord("M") | ord("P") << 8
# pico-sdk puts the binary info header within the image's first 256 words.
BINARY_INFO_HEADER_SEARCH_BYTES = 1024

# Parameters MicroPython's f_mkfs uses (lib/oofatfs/ff.c), so the volume is
# the one the harness would make for itself.
FAT_COUNT = 1
ROOT_ENTRIES = 512
DIR_ENTRY_BYTES = 32
FAT12_MAX_CLUSTERS = 4084


class StorageError(RuntimeError):
    pass


def _word(image: bytes, address: int) -> int:
    offset = address - XIP_BASE
    if not 0 <= offset <= len(image) - 4:
        raise StorageError(f"binary info points outside the image at 0x{address:08x}")
    return struct.unpack_from("<I", image, offset)[0]


# MicroPython declares its filesystem's flash range in the image's binary info.
def storage_region(image: bytes) -> tuple[int, int]:
    for offset in range(0, min(len(image), BINARY_INFO_HEADER_SEARCH_BYTES) - 20, 4):
        start_marker, start, end, _, end_marker = struct.unpack_from("<5I", image, offset)
        if start_marker == BINARY_INFO_MARKER_START and end_marker == BINARY_INFO_MARKER_END:
            break
    else:
        raise StorageError("the image carries no binary info")
    for entry in range(start, end, 4):
        record = _word(image, entry)
        kind_and_tag = _word(image, record)
        if kind_and_tag == BINARY_INFO_TYPE_BLOCK_DEVICE | BINARY_INFO_TAG_MICROPYTHON << 16:
            return _word(image, record + 8) - XIP_BASE, _word(image, record + 12)
    raise StorageError("the image declares no MicroPython block device")


def fat_volume(size: int) -> bytes:
    sectors = size // SECTOR_BYTES
    root_sectors = ROOT_ENTRIES * DIR_ENTRY_BYTES // SECTOR_BYTES
    fat_sectors = math.ceil((sectors + 2) * 3 / 2 / SECTOR_BYTES)
    clusters = sectors - 1 - FAT_COUNT * fat_sectors - root_sectors
    if size % SECTOR_BYTES or not 0 < clusters <= FAT12_MAX_CLUSTERS:
        raise StorageError(f"{size} bytes is not a FAT12 volume of {SECTOR_BYTES}-byte sectors")

    boot = bytearray(SECTOR_BYTES)
    struct.pack_into(
        "<3s8sHBHBHHBHHHII", boot, 0,
        b"\xeb\xfe\x90", b"MSDOS5.0", SECTOR_BYTES, 1, 1, FAT_COUNT, ROOT_ENTRIES,
        sectors, 0xF8, fat_sectors, 63, 255, 0, 0)
    struct.pack_into("<BBBI11s8s", boot, 36, 0x80, 0, 0x29, 0, b"NO NAME    ", b"FAT12   ")
    boot[510:512] = b"\x55\xaa"

    fat = bytearray(fat_sectors * SECTOR_BYTES)
    fat[0:3] = b"\xf8\xff\xff"
    head = bytes(boot) + bytes(fat) * FAT_COUNT + bytes(root_sectors * SECTOR_BYTES)
    return head + bytes([ERASED]) * (clusters * SECTOR_BYTES)


def with_storage(image: bytes) -> bytes:
    base, size = storage_region(image)
    if len(image) > base:
        raise StorageError(f"the image's {len(image)} bytes run into its storage at 0x{base:x}")
    return image + bytes([ERASED]) * (base - len(image)) + fat_volume(size)
