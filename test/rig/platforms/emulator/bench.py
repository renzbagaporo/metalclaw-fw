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
# Linux gives up enumerating after about 90 s of descriptor reads that time
# out; past this, an import with no device behind it is replugged.
REPLUG_AFTER_S = 120.0


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


VHCI_STATUS = Path("/sys/devices/platform/vhci_hcd.0/status")
VHCI_PORT_USED = 6


# The vhci ports holding an import, from the kernel's own table: `usbip port`
# stopped listing an import once the kernel gave up enumerating it.
def _imported_ports() -> list[int]:
    ports = []
    for line in VHCI_STATUS.read_text().splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 3 and int(fields[2]) == VHCI_PORT_USED:
            ports.append(int(fields[1]))
    return ports


def _enumerated(serial: str) -> bool:
    for device in Path("/sys/bus/usb/devices").iterdir():
        try:
            if (device / "idVendor").read_text().strip() == "2fe3" and (device / "serial").read_text().strip() == serial:
                return True
        except OSError:
            continue
    return False


class Cable(threading.Thread):
    # Re-enumeration (attaching a USB-facing device) unplugs the harness; on
    # the bench the cable stays in, so the import is renewed whenever the
    # exported bus is back and not imported. The kernel's descriptor timeouts
    # run in host time and the emulated harness is much slower, so while it
    # is busy the kernel can give up on it for good; then the cable is pulled
    # and plugged again, as on the bench.
    def __init__(self, busid: str, port: int, serial: str, log):
        super().__init__(daemon=True)
        self.target = f"usbip://127.0.0.1:{port}/{busid}"
        self.base = ["sudo", "-n", "usbip", "--tcp-port", str(port)]
        self.busid = busid
        self.serial = serial
        self.log = log
        self.stopping = threading.Event()
        self.imported = threading.Event()
        self.unenumerated_since = None

    def _usbip(self, *args):
        try:
            r = subprocess.run(self.base + list(args), capture_output=True, text=True, timeout=5)
            return r.returncode, r.stdout + r.stderr
        except subprocess.TimeoutExpired:
            return 124, "usbip timed out"

    def run(self):
        while not self.stopping.is_set():
            ports = _imported_ports()
            if not ports:
                self.unenumerated_since = None
                status, detail = self._usbip("attach", "-r", "127.0.0.1", "-b", self.busid)
                if status == 0:
                    self._note(f"imported {self.target}")
                    self.imported.set()
            else:
                self.imported.set()
                now = time.monotonic()
                if _enumerated(self.serial):
                    self.unenumerated_since = None
                elif self.unenumerated_since is None:
                    self.unenumerated_since = now
                elif now - self.unenumerated_since > REPLUG_AFTER_S:
                    self._note(f"not enumerated after {REPLUG_AFTER_S:.0f} s; replugging")
                    self.unenumerated_since = None
                    self._detach(ports)
            self.stopping.wait(0.25)

    def _note(self, message):
        self.log.write(f"usbip: {message}\n")
        self.log.flush()

    def _detach(self, ports):
        for port in ports:
            self._usbip("detach", "-p", str(port))

    def stop(self):
        self.stopping.set()
        self.join(timeout=5)
        self._detach(_imported_ports())


class Bench:
    def __init__(self, renode_dir: Path, harness_uf2: Path, harness_rom: Path, dut_rom: Path, work: Path,
                 serial: str):
        self.renode_dir = renode_dir
        self.serial = serial
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
        self.cable = Cable(USBIP_BUSID, USBIP_PORT, self.serial, self.log)
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
