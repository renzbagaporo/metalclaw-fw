# SPDX-License-Identifier: Apache-2.0 OR MIT
# The host harness process (core/ports/host): MicroPython's unix port with
# the metalclaw module. Its REPL is on one pseudo-terminal and the rig's on
# another, both raw, with this process copying between their two master
# sides: the rig opens a terminal path, as it opens a harness's USB serial
# port, and reads exactly what MicroPython writes.

import os
import pty
import select
import subprocess
import threading
import time
import tty
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]


def host_harness_program() -> Path:
    build = Path(os.environ.get("METALCLAW_BUILD_DIR", ROOT / "build"))
    return Path(os.environ.get("METALCLAW_HOST_HARNESS", build / "host" / "mpy" / "metalclaw-host"))


def _raw_pty() -> tuple[int, int]:
    master, slave = pty.openpty()
    tty.setraw(slave)
    return master, slave


class HostProcess:
    """The host harness, bridged at `socket_path` (None for standalone), its
    REPL at the terminal `self.path`."""

    def __init__(self, program: Path, socket_path: Path | None = None, log: Path | None = None,
                 timeout: float = 10.0):
        env = dict(os.environ)
        if socket_path is None:
            env.pop("METALCLAW_BRIDGE", None)
        else:
            env["METALCLAW_BRIDGE"] = str(socket_path)
        self._harness_master, harness_slave = _raw_pty()
        self._rig_master, self._rig_slave = _raw_pty()
        self.path = os.ttyname(self._rig_slave)
        self._log = open(log, "ab") if log is not None else subprocess.DEVNULL
        self.proc = subprocess.Popen([str(program)], stdin=harness_slave, stdout=harness_slave,
                                     stderr=self._log, env=env, close_fds=True, start_new_session=True)
        os.close(harness_slave)
        self._wait_prompt(timeout)
        self._stop_r, self._stop_w = os.pipe()
        self._thread = threading.Thread(target=self._forward, daemon=True)
        self._thread.start()

    # MicroPython sets its terminal up as it starts, which discards input
    # waiting there; the rig's first bytes go only after its first prompt.
    def _wait_prompt(self, timeout: float) -> None:
        got = b""
        deadline = time.monotonic() + timeout
        while not got.endswith(b">>> "):
            left = deadline - time.monotonic()
            ready, _, _ = select.select([self._harness_master], [], [], max(left, 0))
            if not ready:
                self.proc.kill()
                raise RuntimeError(f"host harness gave no prompt: {got[-300:]!r}")
            got += os.read(self._harness_master, 4096)

    # Bytes each way between the two masters. The rig's slave stays open
    # here, so its terminal outlives the rig closing it between sessions.
    def _forward(self) -> None:
        pairs = {self._harness_master: self._rig_master, self._rig_master: self._harness_master}
        while True:
            ready, _, _ = select.select([*pairs, self._stop_r], [], [])
            if self._stop_r in ready:
                return
            for fd in ready:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                os.write(pairs[fd], data)

    def alive(self) -> bool:
        return self.proc.poll() is None

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        os.write(self._stop_w, b"x")
        self._thread.join(timeout=5)
        for fd in (self._harness_master, self._rig_master, self._rig_slave, self._stop_r, self._stop_w):
            os.close(fd)
        if self._log is not subprocess.DEVNULL:
            self._log.close()
