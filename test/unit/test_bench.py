# SPDX-License-Identifier: Apache-2.0 OR MIT

import socket
from pathlib import Path

import pytest

from test.rig.platforms.emulator import bench


def bench_files(root):
    for name in ("renode", bench.BENCH_SCRIPT):
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).touch()
    for name in ("harness.uf2", "harness_rom.elf", "dut_rom.bin", "zephyr.elf"):
        (root / name).touch()
    return bench.Bench(root, root / "harness.uf2", root / "harness_rom.elf", root / "dut_rom.bin",
                       root / "zephyr.elf", root / "work", "0123456789ABCDEF")


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def commands(quantum=None):
    return bench.monitor_commands(Path("/renode/bench.resc"), Path("/w/rom.bin"), Path("/w/flash.bin"),
                                  Path("/roms/dut.bin"), Path("/w/dut_flash.bin"), quantum)


def test_a_bench_refuses_to_start_beside_another_renode(tmp_path, monkeypatch):
    b = bench_files(tmp_path)
    with socket.socket() as server:
        server.bind(("127.0.0.1", 0))
        server.listen()
        monkeypatch.setattr(bench, "USBIP_PORT", server.getsockname()[1])
        with pytest.raises(bench.BenchError, match="already in use; another Renode is running"):
            b.start()
    assert b.process is None


def test_a_bench_refuses_to_start_without_the_dut_image(tmp_path):
    b = bench_files(tmp_path)
    b.dut_image.unlink()
    with pytest.raises(bench.BenchError, match="missing .*zephyr.elf"):
        b.start()
    assert b.process is None


def test_a_bench_refuses_a_dut_image_it_cannot_load_into_flash(tmp_path, monkeypatch):
    monkeypatch.setattr(bench, "MONITOR_PORT", free_port())
    monkeypatch.setattr(bench, "USBIP_PORT", free_port())
    b = bench_files(tmp_path)
    b.dut_image.write_bytes(b"not an elf")
    with pytest.raises(bench.BenchError, match="zephyr.elf: the DUT image is not a little-endian 32-bit ELF"):
        b.start()
    assert b.process is None


def test_the_bench_script_gets_its_files_before_it_is_included():
    run = commands()
    include = run.index("include @/renode/bench.resc")
    assert set(run[:include]) == {
        "$harness_rom=@/w/rom.bin",
        "$harness_flash=@/w/flash.bin",
        "$dut_rom=@/roms/dut.bin",
        f"$usbip_port={bench.USBIP_PORT}",
    }


def test_the_dut_image_is_loaded_into_the_dut_flash_before_the_bench_starts():
    run = commands()
    assert run[run.index("include @/renode/bench.resc") + 1:] == [
        'mach set "pico2"',
        "xipMemory LoadBinary @/w/dut_flash.bin",
        "start",
    ]


def test_a_quantum_is_set_before_the_bench_script_is_included():
    run = commands("0.001")
    assert run.index('$quantum="0.001"') < run.index("include @/renode/bench.resc")
    assert not any(command.startswith("$quantum") for command in commands())
