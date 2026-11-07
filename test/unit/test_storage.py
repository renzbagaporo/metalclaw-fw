# SPDX-License-Identifier: Apache-2.0 OR MIT

import struct

import pytest

from test.rig.platforms.emulator import storage

STORAGE_BASE = 0x10000
STORAGE_BYTES = 0x10000


def image(length=0x400, base=STORAGE_BASE, tag=storage.BINARY_INFO_TAG_MICROPYTHON):
    data = bytearray([storage.ERASED]) * length
    xip = storage.XIP_BASE
    struct.pack_into("<5I", data, 0x100, storage.BINARY_INFO_MARKER_START, xip + 0x200, xip + 0x208, 0,
                     storage.BINARY_INFO_MARKER_END)
    struct.pack_into("<II", data, 0x200, xip + 0x300, xip + 0x320)
    struct.pack_into("<HHIII", data, 0x300, 1, storage.BINARY_INFO_TAG_MICROPYTHON, 0, 0, 0)
    struct.pack_into("<HHIIIIH", data, 0x320, storage.BINARY_INFO_TYPE_BLOCK_DEVICE, tag, 0, xip + base,
                     STORAGE_BYTES, 0, 3)
    return bytes(data)


def test_the_storage_region_comes_from_the_images_block_device():
    assert storage.storage_region(image()) == (STORAGE_BASE, STORAGE_BYTES)


def test_another_tags_block_device_is_not_the_storage():
    with pytest.raises(storage.StorageError, match="no MicroPython block device"):
        storage.storage_region(image(tag=0x1234))


def test_an_image_without_binary_info_is_refused():
    with pytest.raises(storage.StorageError, match="no binary info"):
        storage.storage_region(bytes(0x400))


def test_the_volume_lands_at_the_storage_base_after_erased_padding():
    seeded = storage.with_storage(image())
    assert len(seeded) == STORAGE_BASE + STORAGE_BYTES
    assert set(seeded[0x400:STORAGE_BASE]) == {storage.ERASED}
    boot = seeded[STORAGE_BASE:STORAGE_BASE + storage.SECTOR_BYTES]
    assert boot[510:512] == b"\x55\xaa"
    assert struct.unpack_from("<HBHBHH", boot, 11) == (
        storage.SECTOR_BYTES, 1, 1, storage.FAT_COUNT, storage.ROOT_ENTRIES, STORAGE_BYTES // storage.SECTOR_BYTES)


def test_the_root_directory_is_empty():
    volume = storage.fat_volume(STORAGE_BYTES)
    fat_sectors = struct.unpack_from("<H", volume, 22)[0]
    root = (1 + storage.FAT_COUNT * fat_sectors) * storage.SECTOR_BYTES
    assert volume[root] == 0


def test_an_image_running_into_its_storage_is_refused():
    with pytest.raises(storage.StorageError, match="run into its storage"):
        storage.with_storage(image(length=0x400, base=0x200))


def test_a_volume_too_large_for_fat12_is_refused():
    with pytest.raises(storage.StorageError, match="not a FAT12 volume"):
        storage.fat_volume(4096 * 5000)
