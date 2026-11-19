# SPDX-License-Identifier: Apache-2.0 OR MIT
# The host harness (core/ports/host) from Python: start it with its REPL on
# a pseudo-terminal, run code in it, and speak the bridge protocol
# (core/ports/host/BRIDGE.md) to it as the emulator holding the DUT does.

import json
import socket
import time
from pathlib import Path

from test.rig.platforms.hardware.repl import Repl
from test.rig.platforms.native.host import HostProcess, host_harness_program


def default_build() -> Path:
    return host_harness_program()


class HostHarness:
    """The host harness process and its raw REPL, as the rig drives a
    harness: `run` returns what the code printed and raises on an error."""

    def __init__(self, program: Path, socket_path: Path | None = None, timeout: float = 10.0,
                 usb_dir: Path | None = None):
        self.process = HostProcess(program, socket_path, usb_dir=usb_dir)
        self.timeout = timeout
        self.repl = Repl(path=self.process.path).__enter__()

    def run(self, code: str) -> str:
        return self.repl.run(code, self.timeout).strip()

    def close(self) -> None:
        try:
            self.repl.__exit__(None, None, None)
        finally:
            self.process.close()


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

    def hello(self, i2c=(), uart=(), spi=()) -> dict:
        return self.request(op="hello", version=1, i2c=list(i2c), uart=list(uart), spi=list(spi))

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


def spi_window(bridge: Bridge, bus: int, cs: int, mosi: bytes) -> bytes:
    """One chip-select window: CS low, a frame per byte, CS high."""
    bridge.request(op="gpio", pin=cs, level=0)
    miso = bytes(bridge.request(op="spi", bus=bus, mosi=b, bits=8)["miso"] for b in mosi)
    bridge.request(op="gpio", pin=cs, level=1)
    return miso
