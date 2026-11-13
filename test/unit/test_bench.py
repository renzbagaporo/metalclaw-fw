# SPDX-License-Identifier: Apache-2.0 OR MIT

import socket
import struct
from pathlib import Path

import pytest

from test.rig.platforms.emulator import bench


def bench_files(root, board="rpi_pi_pico"):
    layout = bench.LAYOUTS[board]
    for name in ("renode", layout.script, "roms/b2.elf", "roms/bootrom-combined.bin"):
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).touch()
    for name in ("harness.uf2", "zephyr.elf"):
        (root / name).touch()
    return bench.Bench(root, layout, root / "harness.uf2", root / "roms", root / "zephyr.elf", root / "work",
                       "0123456789ABCDEF")


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def commands(quantum=None, dut_machine="pico2"):
    return bench.monitor_commands(Path("/renode/bench.resc"), Path("/w/rom.bin"), Path("/w/flash.bin"),
                                  Path("/roms/dut.bin"), Path("/w/dut_flash.bin"), dut_machine, quantum)


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


def test_the_dut_flash_is_loaded_into_the_layouts_dut_machine():
    assert commands(dut_machine="pico")[-3] == 'mach set "pico"'


def test_each_harness_board_has_a_bench_with_its_chips():
    assert bench.LAYOUTS["rpi_pi_pico"] == bench.Layout(
        "scripts/multi-node/metalclaw_fixture.resc", "rp2040", "rp2350", "pico2", "0123456789ABCDEF")
    carrier = bench.LAYOUTS["metalclaw_pico_rp2"]
    assert (carrier.script, carrier.harness_chip, carrier.dut_chip, carrier.dut_machine) == (
        "scripts/multi-node/metalclaw_carrier.resc", "rp2350", "rp2040", "pico")
    assert carrier.serial == "00000000C0FFEE01"


@pytest.mark.parametrize("board", sorted(bench.LAYOUTS))
def test_a_bench_refuses_to_start_without_its_script(tmp_path, board):
    b = bench_files(tmp_path, board)
    (tmp_path / b.layout.script).unlink()
    with pytest.raises(bench.BenchError, match=f"missing .*{b.layout.script}"):
        b.start()
    assert b.process is None


@pytest.mark.parametrize("board, rom", [("rpi_pi_pico", "bootrom-combined.bin"), ("metalclaw_pico_rp2", "b2.elf")])
def test_a_bench_refuses_to_start_without_its_duts_boot_rom(tmp_path, board, rom):
    b = bench_files(tmp_path, board)
    (tmp_path / "roms" / rom).unlink()
    with pytest.raises(bench.BenchError, match=f"missing .*roms/{rom}"):
        b.start()


def test_a_bench_refuses_a_harness_uf2_for_another_chip(tmp_path, monkeypatch):
    monkeypatch.setattr(bench, "MONITOR_PORT", free_port())
    monkeypatch.setattr(bench, "USBIP_PORT", free_port())
    monkeypatch.setattr(bench.dut_flash, "flash_contents", lambda elf: b"\xff")
    b = bench_files(tmp_path, "metalclaw_pico_rp2")
    b.harness_uf2.write_bytes(uf2_block(0x10000000, 0xE48BFF56))
    with pytest.raises(bench.BenchError, match="harness.uf2: the UF2 has no rp2350 flash blocks"):
        b.start()
    assert b.process is None


def uf2_block(address, family, flags=0x2000, payload=b"\x00" * 256):
    header = struct.pack("<8I", 0x0A324655, 0x9E5D5157, flags, address, len(payload), 0, 1, family)
    return header + payload + bytes(512 - 32 - len(payload) - 4) + struct.pack("<I", 0x0AB16F30)


class Launched(Exception):
    pass


# Starts a bench up to launching Renode and returns Renode's monitor commands.
def launch(tmp_path, monkeypatch, board):
    monkeypatch.setattr(bench, "MONITOR_PORT", free_port())
    monkeypatch.setattr(bench, "USBIP_PORT", free_port())
    monkeypatch.setattr(bench.dut_flash, "flash_contents", lambda elf: b"dut")
    monkeypatch.setattr(bench.storage, "with_storage", lambda image: image + b"fat")
    extracted = []

    def extract(argv, **kwargs):
        extracted.append(Path(argv[2]).name)
        Path(argv[3]).write_bytes(b"mask rom")

    def popen(argv, **kwargs):
        raise Launched([argv[i + 1] for i, arg in enumerate(argv) if arg == "-e"])

    monkeypatch.setattr(bench.subprocess, "run", extract)
    monkeypatch.setattr(bench.subprocess, "Popen", popen)
    b = bench_files(tmp_path, board)
    chip = b.layout.harness_chip
    b.harness_uf2.write_bytes(uf2_block(0x10000000, {"rp2040": 0xE48BFF56, "rp2350": 0xE48BFF59}[chip]))
    with pytest.raises(Launched) as launched:
        b.start()
    b.log.close()
    assert (b.work / "harness_flash.bin").read_bytes() == bytes(256) + b"fat"
    assert (b.work / "dut_flash.bin").read_bytes() == b"dut"
    return launched.value.args[0], extracted


def test_the_fixture_1_bench_gives_the_rp2040_mask_rom_to_the_harness(tmp_path, monkeypatch):
    run, extracted = launch(tmp_path, monkeypatch, "rpi_pi_pico")
    assert extracted == ["b2.elf"]
    assert f"$harness_rom=@{tmp_path}/work/harness_rom.bin" in run
    assert f"$dut_rom=@{tmp_path}/roms/bootrom-combined.bin" in run
    assert f"include @{tmp_path}/scripts/multi-node/metalclaw_fixture.resc" in run
    assert 'mach set "pico2"' in run


def test_the_carrier_bench_gives_the_rp2040_mask_rom_to_the_dut(tmp_path, monkeypatch):
    run, extracted = launch(tmp_path, monkeypatch, "metalclaw_pico_rp2")
    assert extracted == ["b2.elf"]
    assert f"$harness_rom=@{tmp_path}/roms/bootrom-combined.bin" in run
    assert f"$dut_rom=@{tmp_path}/work/dut_rom.bin" in run
    assert f"include @{tmp_path}/scripts/multi-node/metalclaw_carrier.resc" in run
    assert 'mach set "pico"' in run
