# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness REPL over its CDC port, addressed by serial, driven in raw mode.

import glob
import time

import serial


class Repl:
    # A harness on USB by its serial number, or the host harness's
    # pseudo-terminal by `path`.
    def __init__(self, serial_number: str | None = None, path: str | None = None):
        if path is None:
            pattern = f"/dev/serial/by-id/usb-Metalclaw_*_{serial_number}-if00"
            ports = glob.glob(pattern)
            if len(ports) != 1:
                raise RuntimeError(f"expected one port matching {pattern}, found {ports}")
            path = ports[0]
        self.port = serial.Serial(path, 115200, timeout=0.1)

    def __enter__(self):
        self.port.reset_input_buffer()
        self.port.write(b"\x03\x03")
        self.port.write(b"\x01")
        deadline = time.monotonic() + 5.0
        entry = b""
        try:
            while not entry.endswith(b"raw REPL; CTRL-B to exit\r\n>"):
                entry += self._read()
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"raw REPL entry prompt timed out: {entry!r}")
        except Exception:
            self.port.close()
            raise
        return self

    def __exit__(self, *exc):
        try:
            self.port.write(b"\x02")
        finally:
            self.port.close()

    # What has arrived, or one byte once it does: a fixed-size read waits out
    # the port timeout whenever the reply is shorter.
    def _read(self) -> bytes:
        return self.port.read(self.port.in_waiting or 1)

    def run(self, code: str, timeout: float = 5.0) -> str:
        self.port.reset_input_buffer()
        self.port.write(code.encode() + b"\x04")
        out = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            out += self._read()
            if out.count(b"\x04") >= 2:
                break
        # The prompt that ended the code before may arrive after it, ahead
        # of this reply (the host harness's terminal is relayed).
        out = out.lstrip(b">")
        if not out.startswith(b"OK"):
            raise RuntimeError(f"raw REPL did not accept the code: {out!r}")
        body, _, err = out[2:].partition(b"\x04")
        err = err.rstrip(b"\x04>")
        if err.strip():
            raise RuntimeError(err.decode(errors="replace").strip())
        return body.decode(errors="replace")

    def soft_reset(self) -> None:
        self.port.write(b"\x02")
        time.sleep(0.2)
        self.port.write(b"\x04")
