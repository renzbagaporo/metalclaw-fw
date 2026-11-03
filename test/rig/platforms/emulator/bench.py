# SPDX-License-Identifier: Apache-2.0 OR MIT
# The bench in Renode: both Picos and their wires in one emulation, with the
# harness's USB exported over USB/IP and imported into this host's kernel, so
# the host sees the same USB device the bench does.

import os
import socket
import subprocess
import threading
import time
from pathlib import Path

BENCH_SCRIPT = "scripts/multi-node/metalclaw_fixture.resc"
UF2_EXTRACTOR = "scripts/multi-node/extract_pico1_uf2.py"
MASK_ROM_EXTRACTOR = "scripts/multi-node/extract_rp2040_mask_rom.py"
MONITOR_PORT = 1234
USBIP_PORT = 3240
USBIP_BUSID = "1-0"
STARTUP_TIMEOUT_S = 120.0


class BenchError(RuntimeError):
    pass


def _wait_port(port: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while True:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError:
            if time.monotonic() >= deadline:
                raise BenchError(f"nothing listens on port {port} after {timeout:.0f} s")
            time.sleep(0.5)


class Cable(threading.Thread):
    # Re-enumeration (attaching a USB-facing device) unplugs the harness; on
    # the bench the cable stays in, so the import is renewed whenever the
    # exported bus is back and not imported.
    def __init__(self, busid: str, port: int, log):
        super().__init__(daemon=True)
        self.target = f"usbip://127.0.0.1:{port}/{busid}"
        self.base = ["sudo", "-n", "usbip", "--tcp-port", str(port)]
        self.busid = busid
        self.log = log
        self.stopping = threading.Event()
        self.imported = threading.Event()

    def _usbip(self, *args):
        try:
            r = subprocess.run(self.base + list(args), capture_output=True, text=True, timeout=5)
            return r.returncode, r.stdout + r.stderr
        except subprocess.TimeoutExpired:
            return 124, "usbip timed out"

    def run(self):
        while not self.stopping.is_set():
            status, ports = self._usbip("port")
            if status == 0 and self.target not in ports:
                status, detail = self._usbip("attach", "-r", "127.0.0.1", "-b", self.busid)
                if status == 0:
                    self.log.write(f"usbip: imported {self.target}\n")
                    self.log.flush()
                    self.imported.set()
            elif status == 0:
                self.imported.set()
            self.stopping.wait(0.25)

    def stop(self):
        self.stopping.set()
        self.join(timeout=5)
        status, ports = self._usbip("port")
        for line in ports.splitlines():
            if line.startswith("Port ") and ":" in line:
                current = line.split()[1].rstrip(":")
            elif self.target in line:
                self._usbip("detach", "-p", current)


class Bench:
    def __init__(self, renode_dir: Path, harness_uf2: Path, harness_rom: Path, dut_rom: Path, work: Path):
        self.renode_dir = renode_dir
        self.harness_uf2 = harness_uf2
        self.harness_rom = harness_rom
        self.dut_rom = dut_rom
        self.work = work
        self.process = None
        self.cable = None
        self.log = None

    # Both extractors are the bench script's own and refuse to overwrite.
    def _extract(self, extractor: str, source: Path, name: str) -> Path:
        out = self.work / name
        out.unlink(missing_ok=True)
        subprocess.run(["python3", str(self.renode_dir / extractor), str(source), str(out)],
                       check=True, capture_output=True, text=True)
        return out

    def start(self) -> None:
        for path in (self.renode_dir / "renode", self.renode_dir / BENCH_SCRIPT, self.harness_uf2,
                     self.harness_rom, self.dut_rom):
            if not path.is_file():
                raise BenchError(f"missing {path}")
        self.work.mkdir(parents=True, exist_ok=True)
        payload = self._extract(UF2_EXTRACTOR, self.harness_uf2, "harness_flash.bin")
        mask_rom = self._extract(MASK_ROM_EXTRACTOR, self.harness_rom, "harness_rom.bin")
        self.log = (self.work / "renode.log").open("w")
        commands = [
            f"$harness_rom=@{mask_rom}",
            f"$harness_flash=@{payload}",
            f"$dut_rom=@{self.dut_rom}",
            f"$usbip_port={USBIP_PORT}",
        ]
        quantum = os.environ.get("METALCLAW_RENODE_QUANTUM")
        if quantum:
            commands.append(f'$quantum="{quantum}"')
        commands += [f"include @{self.renode_dir / BENCH_SCRIPT}", "start"]
        argv = [str(self.renode_dir / "renode"), "--disable-gui", "--plain", "--port", str(MONITOR_PORT)]
        for command in commands:
            argv += ["-e", command]
        self.process = subprocess.Popen(argv, cwd=self.renode_dir, stdout=self.log, stderr=subprocess.STDOUT,
                                        start_new_session=True)
        _wait_port(USBIP_PORT, STARTUP_TIMEOUT_S)
        self.cable = Cable(USBIP_BUSID, USBIP_PORT, self.log)
        self.cable.start()
        if not self.cable.imported.wait(STARTUP_TIMEOUT_S):
            raise BenchError(f"the harness's USB/IP bus {USBIP_BUSID} was never imported")

    def stop(self) -> None:
        if self.cable is not None:
            self.cable.stop()
        if self.process is not None and self.process.poll() is None:
            try:
                with socket.create_connection(("127.0.0.1", MONITOR_PORT), timeout=5) as monitor:
                    monitor.sendall(b"quit\n")
                self.process.wait(timeout=30)
            except (OSError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait()
        if self.log is not None:
            self.log.close()
