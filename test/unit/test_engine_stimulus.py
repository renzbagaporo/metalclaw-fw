# SPDX-License-Identifier: Apache-2.0 OR MIT
# The stimulus against a stand-in for the emulated harness: a client that
# plays the emulator's side of the bridge, applying the stimulus's timed
# actions to its lines and answering with an I2C target at the bit level.

import json
import socket
import threading
import time

from test.engine.stimulus import I2cController, Stimulus

SDA, SCL = 4, 5


class FakeTarget:
    """A bit-level I2C target at `address`: a write's first byte sets the
    register pointer, later ones store; a read returns from the pointer."""

    def __init__(self, address: int, regs: dict[int, int]):
        self.address, self.regs = address, dict(regs)
        self.state, self.i, self.shift, self.ptr, self.out = "idle", 0, 0, 0, 0
        self.first = True
        self.drive = None  # 0 or None (released)

    def start(self):
        self.state, self.i, self.shift, self.drive, self.first = "addr", 0, 0, None, True

    def stop(self):
        self.state, self.drive = "idle", None

    # Bits count on SCL's rising edges; the target changes SDA only while
    # SCL is low, on its falling edges.
    def rise(self, sda: int):
        if self.state in ("addr", "write", "read") and self.i < 8:
            if self.state != "read":
                self.shift = (self.shift << 1) | sda
            self.i += 1
        elif self.state in ("addr", "write", "read") and self.i == 8:
            self.master_ack = sda == 0
            self.i = 9

    def _present(self):
        self.drive = None if (self.out >> (7 - self.i)) & 1 else 0

    def fall(self):
        if self.state in ("addr", "write"):
            if self.i == 8:
                ours = self.state == "write" or (self.shift >> 1) == self.address
                self.drive = 0 if ours else None
                if not ours:
                    self.state = "idle"
            elif self.i == 9:
                self.drive, self.i = None, 0
                byte, self.shift = self.shift & 0xFF, 0
                if self.state == "addr":
                    if byte & 1:
                        self.state, self.out = "read", self.regs.get(self.ptr, 0xFF)
                        self.ptr += 1
                        self._present()
                    else:
                        self.state = "write"
                elif self.first:
                    self.ptr, self.first = byte, False
                else:
                    self.regs[self.ptr] = byte
                    self.ptr += 1
        elif self.state == "read":
            if 0 < self.i < 8:
                self._present()
            elif self.i == 8:
                self.drive = None
            elif self.i == 9:
                if self.master_ack:
                    self.out, self.i = self.regs.get(self.ptr, 0xFF), 0
                    self.ptr += 1
                    self._present()
                else:
                    self.state, self.drive = "idle", None


class FakeHarness(threading.Thread):
    """The emulator's side of the bridge: jumps from one timed action (or
    wake, or the next millisecond) to the next, applies it to the lines,
    and reports the target's drive on SDA as the emulator would."""

    def __init__(self, path, target: FakeTarget, run_ns: int):
        super().__init__(daemon=True)
        self.path, self.target, self.run_ns = path, target, run_ns
        self.t = 0
        self.pending: list[dict] = []
        self.stim_level = {SDA: None, SCL: None}
        self.error = None

    def line(self, pin):
        own = self.target.drive if pin == SDA else None
        return 0 if 0 in (own, self.stim_level[pin]) else 1

    def request(self, stream, msg):
        msg["t_ns"] = self.t
        stream.write(json.dumps(msg).encode() + b"\n")
        stream.flush()
        reply = json.loads(stream.readline())
        self.pending += reply.get("actions", [])
        self.pending.sort(key=lambda a: a["t_ns"])

    def run(self):
        try:
            for _ in range(200):
                try:
                    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    sock.connect(str(self.path))
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    sock.close()
                    time.sleep(0.01)
            with sock, sock.makefile("rwb") as stream:
                self.request(stream, {"op": "hello", "version": 1, "i2c": [], "uart": [], "spi": []})
                while self.t < self.run_ns:
                    nxt = min([a["t_ns"] for a in self.pending] + [(self.t // 1_000_000 + 1) * 1_000_000])
                    self.t = max(self.t, nxt)
                    before = self.target.drive
                    while self.pending and self.pending[0]["t_ns"] <= self.t:
                        self.apply(self.pending.pop(0))
                    if self.target.drive != before:
                        self.request(stream, {"op": "gpio", "pin": SDA,
                                              "level": "z" if self.target.drive is None else 0})
                    else:
                        self.request(stream, {"op": "tick"})
        except BaseException as error:  # noqa: BLE001
            self.error = error

    def apply(self, action):
        if action["op"] != "gpio":
            return
        pin = action["pin"]
        sda, scl = self.line(SDA), self.line(SCL)
        self.stim_level[pin] = None if action["level"] == "z" else action["level"]
        new_sda, new_scl = self.line(SDA), self.line(SCL)
        if pin == SDA and scl and new_sda != sda:
            (self.target.start if new_sda == 0 else self.target.stop)()
        elif pin == SCL and new_scl != scl:
            (self.target.rise(new_sda) if new_scl else self.target.fall())


def _run(tmp_path, target, body):
    stim = Stimulus(tmp_path / "engine.sock")
    harness = FakeHarness(stim.path, target, run_ns=50_000_000)
    harness.start()
    try:
        return body(stim)
    finally:
        stim.close()
        harness.join(timeout=10)
        assert harness.error is None or isinstance(harness.error, (OSError, ValueError)), harness.error


def test_an_i2c_controller_reads_a_register_bit_by_bit(tmp_path):
    target = FakeTarget(0x76, {0x00: 0x50, 0x01: 0xA5})

    def body(stim):
        bus = I2cController(stim, SDA, SCL)
        return bus.transfer(0x76, write=b"\x00", read=2)
    result = _run(tmp_path, target, body)
    assert result.acked and result.data == b"\x50\xa5"


def test_an_i2c_controller_writes_and_sees_a_nack(tmp_path):
    target = FakeTarget(0x76, {})

    def body(stim):
        bus = I2cController(stim, SDA, SCL)
        wrote = bus.transfer(0x76, write=b"\x10\x2a")
        missing = bus.transfer(0x77, write=b"\x10")
        return wrote, missing
    wrote, missing = _run(tmp_path, target, body)
    assert wrote.acked and target.regs[0x10] == 0x2A
    assert not missing.acked
