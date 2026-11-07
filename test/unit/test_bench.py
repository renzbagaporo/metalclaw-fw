# SPDX-License-Identifier: Apache-2.0 OR MIT

import socket

import pytest

from test.rig.platforms.emulator import bench


def test_a_bench_refuses_to_start_beside_another_renode(tmp_path, monkeypatch):
    for name in ("renode", bench.BENCH_SCRIPT):
        (tmp_path / name).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / name).touch()
    for name in ("harness.uf2", "harness_rom.elf", "dut_rom.bin"):
        (tmp_path / name).touch()
    with socket.socket() as server:
        server.bind(("127.0.0.1", 0))
        server.listen()
        monkeypatch.setattr(bench, "USBIP_PORT", server.getsockname()[1])
        b = bench.Bench(tmp_path, tmp_path / "harness.uf2", tmp_path / "harness_rom.elf", tmp_path / "dut_rom.bin",
                        tmp_path / "work", "0123456789ABCDEF")
        with pytest.raises(bench.BenchError, match="already in use; another Renode is running"):
            b.start()
    assert b.process is None
