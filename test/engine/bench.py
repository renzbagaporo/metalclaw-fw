# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness alone in Renode, for its engines: the emulator bench's harness
# (its firmware, its USB over USB/IP for the REPL), with no DUT; a bridge on
# its GPIO bank connects to a native stimulus (stimulus.py), which drives its
# pins as a DUT would.

import os
import subprocess
from dataclasses import dataclass
from pathlib import Path

from test.rig.platforms.emulator import storage, uf2
from test.rig.platforms.emulator.bench import (
    MONITOR_PORT, ROMS, STARTUP_TIMEOUT_S, USBIP_BUSID, USBIP_PORT, Bench, BenchError, Cable, _listening,
    _wait_port, renode_argv,
)


@dataclass(frozen=True)
class HarnessLayout:
    script: str
    chip: str
    # The USB serial the emulated harness reports, and the script variable
    # that sets it, where it has one.
    serial: str
    chip_id_variable: str | None = None


LAYOUTS = {
    "rpi_pi_pico": HarnessLayout("scripts/multi-node/metalclaw_harness_native.resc", "rp2040", "0123456789ABCDEF"),
    "metalclaw_pico_rp2": HarnessLayout("scripts/multi-node/metalclaw_harness_rp2_native.resc", "rp2350",
                                        "00000000C0FFEE01", "harness_chip_id"),
}


class EngineBench(Bench):
    """The harness of `layout` in Renode, bridged at `bridge` (the
    stimulus's socket); start() returns once its USB is imported."""

    def __init__(self, renode_dir: Path, layout: HarnessLayout, harness_uf2: Path, roms: Path, work: Path,
                 bridge: Path, instance: int = 0):
        super().__init__(renode_dir, layout, harness_uf2, roms, harness_uf2, work, layout.serial, instance)
        self.bridge = bridge

    def _start(self) -> None:
        layout = self.layout
        for path in (self.renode_dir / "renode", self.renode_dir / layout.script, self.harness_uf2,
                     self.roms / ROMS[layout.chip]):
            if not path.is_file():
                raise BenchError(f"missing {path}")
        monitor_port, usbip_port = MONITOR_PORT + self.instance, USBIP_PORT + self.instance
        for port in (monitor_port, usbip_port):
            if _listening(port):
                raise BenchError(f"port {port} is already in use; another Renode is running")
        try:
            flash = storage.with_storage(uf2.flash_contents(self.harness_uf2.read_bytes(), layout.chip))
        except (uf2.Uf2Error, storage.StorageError) as error:
            raise BenchError(f"{self.harness_uf2}: {error}") from error
        self.work.mkdir(parents=True, exist_ok=True)
        payload = self.work / "harness_flash.bin"
        payload.write_bytes(flash)
        rom = self._rom(layout.chip, "harness_rom.bin")
        commands = [f"$harness_rom=@{rom}", f"$harness_flash=@{payload}", f"$usbip_port={usbip_port}"]
        if layout.chip_id_variable is not None:
            commands.append(f"${layout.chip_id_variable}=0x{layout.serial}")
        commands += [f"include @{self.renode_dir / layout.script}", "start"]
        self.log = (self.work / "renode.log").open("w")
        self._hold_exits()
        env = dict(os.environ, METALCLAW_BRIDGE=str(self.bridge))
        self.process = subprocess.Popen(renode_argv(self.renode_dir / "renode", monitor_port, commands),
                                        cwd=self.renode_dir, stdout=self.log, stderr=subprocess.STDOUT,
                                        start_new_session=True, env=env)
        self._ended = False
        self.monitor_port = monitor_port
        self.usbip_port = usbip_port
        _wait_port(usbip_port, STARTUP_TIMEOUT_S)
        self.cable = Cable(USBIP_BUSID, usbip_port, self.serial, self.log)
        self.cable.start()
        if not self.cable.imported.wait(STARTUP_TIMEOUT_S):
            raise BenchError(f"the harness's USB/IP bus {USBIP_BUSID} was never imported")
