# SPDX-License-Identifier: Apache-2.0 OR MIT
# The host harness (core/ports/host) from Python: start it with its REPL on
# a pseudo-terminal, run code in it, and speak the bridge protocol
# (core/ports/host/BRIDGE.md) to it as the emulator holding the DUT does.

import json
import os
import pty
import select
import socket
import subprocess
import time
from pathlib import Path

PROMPT = b">>> "


def default_build() -> Path:
    root = Path(__file__).resolve().parents[2]
    build = Path(os.environ.get("METALCLAW_BUILD_DIR", root / "build"))
    return Path(os.environ.get("METALCLAW_HOST_HARNESS", build / "host" / "mpy" / "metalclaw-host"))


class HostHarness:
    """The host harness process, its REPL on a pty, bridged to `socket_path`
    (None for standalone)."""

    def __init__(self, program: Path, socket_path: Path | None = None, timeout: float = 10.0):
        env = dict(os.environ)
        if socket_path is None:
            env.pop("METALCLAW_BRIDGE", None)
        else:
            env["METALCLAW_BRIDGE"] = str(socket_path)
        self.timeout = timeout
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen([str(program)], stdin=slave, stdout=slave, stderr=slave, env=env,
                                     close_fds=True)
        os.close(slave)
        self._read_until(PROMPT)

    def _read_until(self, marker: bytes) -> bytes:
        got = b""
        deadline = time.monotonic() + self.timeout
        while not got.endswith(marker):
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError(f"host harness: no {marker!r} after {got[-400:]!r}")
            ready, _, _ = select.select([self.master], [], [], left)
            if ready:
                chunk = os.read(self.master, 4096)
                if not chunk:
                    raise EOFError(f"host harness exited: {got[-400:]!r}")
                got += chunk
        return got

    def run(self, code: str) -> str:
        """Runs `code` (one statement per line) at the REPL; returns what it
        printed. A traceback raises."""
        printed = []
        for line in code.strip().splitlines():
            os.write(self.master, line.encode() + b"\r")
            got = self._read_until(PROMPT).decode(errors="replace").replace("\r\n", "\n")
            # The terminal echoes the line first; the prompt ends the reply.
            body = got[: -len(PROMPT)]
            body = body.split("\n", 1)[1] if "\n" in body else ""
            if "Traceback (most recent call last)" in body:
                raise RuntimeError(f"host harness: {line}\n{body}")
            printed.append(body)
        return "".join(printed).strip()

    def close(self) -> None:
        self.proc.kill()
        self.proc.wait()
        os.close(self.master)


class Bridge:
    """The emulator's end of the bridge: one request at a time, each with
    the DUT's virtual time, each answered before the next."""

    def __init__(self, socket_path: Path, timeout: float = 10.0):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(str(socket_path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                self.sock.close()
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)
        self.sock.settimeout(timeout)
        self.buf = b""
        self.t_ns = 0
        self.actions: list[dict] = []

    def request(self, **msg) -> dict:
        msg.setdefault("t_ns", self.t_ns)
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("bridge closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        reply = json.loads(line)
        self.actions.extend(reply.get("actions", ()))
        if "error" in reply:
            raise RuntimeError(f"bridge: {reply['error']} for {msg}")
        return reply

    def advance(self, ns: int) -> dict:
        self.t_ns += ns
        return self.request(op="tick")

    def hello(self, i2c=(), uart=()) -> dict:
        return self.request(op="hello", version=1, i2c=list(i2c), uart=list(uart))

    # An I2C transaction as the DUT's controller makes it: a write of
    # `write` bytes, then, after a repeated START, a read of `read` bytes.
    def i2c(self, bus: int, address: int, write: bytes = b"", read: int = 0) -> bytes | None:
        if write or not read:
            if not self.request(op="i2c", bus=bus, ev="start", addr=address, read=False, repeated=False)["ack"]:
                self.request(op="i2c", bus=bus, ev="stop")
                return None
            for b in write:
                if not self.request(op="i2c", bus=bus, ev="write", data=b)["ack"]:
                    self.request(op="i2c", bus=bus, ev="stop")
                    return None
        data = b""
        if read:
            if not self.request(op="i2c", bus=bus, ev="start", addr=address, read=True,
                                repeated=bool(write))["ack"]:
                self.request(op="i2c", bus=bus, ev="stop")
                return None
            data = bytes(self.request(op="i2c", bus=bus, ev="read")["data"] for _ in range(read))
        self.request(op="i2c", bus=bus, ev="stop")
        return data

    def close(self) -> None:
        self.sock.close()
