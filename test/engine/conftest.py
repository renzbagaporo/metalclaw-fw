# SPDX-License-Identifier: Apache-2.0 OR MIT
# The engine cases' bench: the harness alone in Renode (bench.py), its pins
# bridged to a native stimulus (stimulus.py), its REPL over USB/IP as on the
# emulator platform. Run with pytest test/engine and:
#   METALCLAW_RENODE_DIR       a Renode package with the harness-only scripts
#   METALCLAW_RENODE_ROMS      the boot ROMs
#   METALCLAW_HARNESS_BOARD    rpi_pi_pico (default) or metalclaw_pico_rp2
#   METALCLAW_HARNESS_IMAGE    the harness's firmware.uf2 (default: the build)
#   METALCLAW_ENGINE_WORK      a directory for the socket and logs
#   METALCLAW_RENODE_INSTANCE  ports per instance, as for the emulator

import os
from pathlib import Path

import pytest

from test.engine.bench import LAYOUTS, EngineBench
from test.engine.stimulus import Stimulus
from test.rig.platforms.emulator.bench import BenchError
from test.rig.platforms.emulator.platform import EmulatedHarness

ROOT = Path(__file__).resolve().parents[2]


def _required(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        pytest.skip(f"set {name} to run the engine cases")
    return value


@pytest.fixture(scope="session")
def board() -> str:
    return os.environ.get("METALCLAW_HARNESS_BOARD", "rpi_pi_pico")


@pytest.fixture(scope="session")
def stimulus(board):
    renode = Path(_required("METALCLAW_RENODE_DIR"))
    roms = Path(_required("METALCLAW_RENODE_ROMS"))
    instance = int(os.environ.get("METALCLAW_RENODE_INSTANCE", "0"))
    work = Path(os.environ.get("METALCLAW_ENGINE_WORK", ROOT / "build/engine")) / str(instance)
    work.mkdir(parents=True, exist_ok=True)
    stim = Stimulus(work / "engine.sock")
    image = Path(os.environ.get("METALCLAW_HARNESS_IMAGE", ROOT / f"build/rp2/{board}/firmware.uf2"))
    bench = EngineBench(renode, LAYOUTS[board], image, roms, work, stim.path, instance)
    try:
        try:
            bench.start()
        except BenchError as error:
            pytest.exit(str(error), returncode=2)
        yield stim
    finally:
        bench.stop()
        stim.close()


@pytest.fixture(scope="session")
def harness(stimulus, board):
    h = EmulatedHarness(LAYOUTS[board].serial)
    h.wait_enumerated()
    return h
