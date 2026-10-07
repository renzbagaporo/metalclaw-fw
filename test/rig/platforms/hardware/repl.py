# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness REPL over its CDC port, addressed by serial, driven in raw mode.

import glob
import time

import serial


class Repl:
    def __init__(self, serial_number: str):
        pattern = f"/dev/serial/by-id/usb-Metalclaw_*_{serial_number}-if00"
        ports = glob.glob(pattern)
        if len(ports) != 1:
            raise RuntimeError(f"expected one port matching {pattern}, found {ports}")
        self.port = serial.Serial(ports[0], 115200, timeout=0.1)

    def __enter__(self):
        self.port.write(b"\x03\x03")
        time.sleep(0.2)
        self.port.write(b"\x01")
        time.sleep(0.2)
        self.port.reset_input_buffer()
        return self

    def __exit__(self, *exc):
        try:
            self.port.write(b"\x02")
        finally:
            self.port.close()

    def run(self, code: str, timeout: float = 5.0) -> str:
        self.port.reset_input_buffer()
        self.port.write(code.encode() + b"\x04")
        out = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            out += self.port.read(4096)
            if out.count(b"\x04") >= 2:
                break
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
