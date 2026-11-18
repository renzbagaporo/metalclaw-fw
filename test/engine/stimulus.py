# SPDX-License-Identifier: Apache-2.0 OR MIT
# A native stimulus for the harness's engines: the bridge's server end
# (core/ports/host/BRIDGE.md) with the emulated harness in the DUT's place.
# The emulator connects, reports every level the harness drives on a watched
# pin with its virtual time, and takes timed gpio actions that drive the pins'
# pad inputs; this side drives the pins as a DUT would, bit by bit.
#
# A test holds the emulator at a virtual time (`until`): the first request at
# or after it waits for its reply while the test reads what the harness drove
# (`harness_level`) and queues the next edges, all later than that request.
# A `wake` action asks for a request at its time, so a hold lands there
# exactly; without one, the emulator's tick (every ms) is the next.

import json
import socket
import threading
from dataclasses import dataclass
from pathlib import Path

RELEASED = "z"


class Stimulus:
    def __init__(self, path: Path, timeout: float = 30.0):
        self.path = path
        self.timeout = timeout
        path.unlink(missing_ok=True)
        self._server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._server.bind(str(path))
        self._server.listen(1)
        self._cond = threading.Condition()
        self.t_ns = 0
        # What the harness drove on each watched pin: (t_ns, 0, 1 or None).
        self.harness: dict[int, list[tuple[int, int | None]]] = {}
        # What this side drove on each pin: (t_ns, 0, 1 or None).
        self.driven: dict[int, list[tuple[int, int | None]]] = {}
        self.pulled: set[int] = set()
        self._actions: list[dict] = []
        self._hold_at: int | None = None
        self._held: int | None = None
        self._closed = False
        self.error: BaseException | None = None
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    # ---- the emulator's side -------------------------------------------------

    def _serve(self) -> None:
        try:
            conn, _ = self._server.accept()
            with conn, conn.makefile("rwb") as stream:
                for line in stream:
                    stream.write(self._answer(json.loads(line)).encode() + b"\n")
                    stream.flush()
        except BaseException as error:  # noqa: BLE001 - surfaced to the test
            with self._cond:
                self.error = error
                self._cond.notify_all()
        finally:
            with self._cond:
                self._closed = True
                self._cond.notify_all()

    def _answer(self, msg: dict) -> str:
        with self._cond:
            t = int(msg.get("t_ns", self.t_ns))
            self.t_ns = max(self.t_ns, t)
            if msg.get("op") == "gpio":
                level = msg.get("level")
                self.harness.setdefault(msg["pin"], []).append((t, level if isinstance(level, int) else None))
            if self._hold_at is not None and t >= self._hold_at:
                self._held = t
                self._hold_at = None
                self._cond.notify_all()
                while self._held is not None and not self._closed:
                    self._cond.wait()
            actions, self._actions = self._actions, []
        reply = {"version": 1} if msg.get("op") == "hello" else {}
        if actions:
            reply["actions"] = actions
        return json.dumps(reply)

    # ---- the test's side ---------------------------------------------------

    def until(self, t_ns: int) -> int:
        """Holds the emulator at the first request at or after t_ns and
        returns its time; the next `until` or `go` lets it run on."""
        with self._cond:
            self._release()
            self._hold_at = t_ns
            self._actions.append({"t_ns": t_ns, "op": "wake"})
            if not self._cond.wait_for(lambda: self._held is not None or self._closed, self.timeout):
                raise TimeoutError(f"the emulator never reached {t_ns} ns (at {self.t_ns})")
            if self.error is not None:
                raise RuntimeError("the bridge failed") from self.error
            if self._held is None:
                raise EOFError("the emulator closed the bridge")
            return self._held

    def go(self) -> None:
        with self._cond:
            self._release()

    def _release(self) -> None:
        self._held = None
        self._cond.notify_all()

    def watch(self, pin: int) -> None:
        self._queue({"t_ns": self.t_ns, "op": "watch", "pin": pin})

    def pull_up(self, pin: int) -> None:
        self.pulled.add(pin)
        self._queue({"t_ns": self.t_ns, "op": "pull", "pin": pin, "level": 1})

    def drive(self, pin: int, level: int | None, t_ns: int) -> None:
        """Drives `pin` to 0 or 1 at t_ns, or releases it (None). A level
        must hold a while: one changed again at the same instant is a pulse
        no input sees (the PIO's synchronizer takes two cycles)."""
        history = self.driven.setdefault(pin, [])
        if history and history[-1][0] >= t_ns and history[-1][1] != level:
            raise ValueError(f"GP{pin} changes at {t_ns} ns, no later than its last change at {history[-1][0]} ns")
        history.append((t_ns, level))
        self._queue({"t_ns": t_ns, "op": "gpio", "pin": pin, "level": RELEASED if level is None else level})

    def _queue(self, action: dict) -> None:
        with self._cond:
            self._actions.append(action)

    @staticmethod
    def _at(history, t_ns: int):
        level = None
        for t, value in history:
            if t > t_ns:
                break
            level = value
        return level

    def harness_level(self, pin: int, t_ns: int) -> int | None:
        """What the harness drove on `pin` at t_ns: 0, 1, or None."""
        with self._cond:
            return self._at(self.harness.get(pin, ()), t_ns)

    def line(self, pin: int, t_ns: int) -> int:
        """An open-drain line's level at t_ns: low while either side holds
        it low, else its pull-up's high."""
        with self._cond:
            sides = (self._at(self.harness.get(pin, ()), t_ns), self._at(self.driven.get(pin, ()), t_ns))
        return 0 if 0 in sides else 1

    def close(self) -> None:
        with self._cond:
            self._closed = True
            self._release()
        self._server.close()
        self.path.unlink(missing_ok=True)


class I2cStretched(Exception):
    """The harness held SCL low through a bit this controller had timed."""


@dataclass
class I2cResult:
    acked: bool
    data: bytes = b""


class I2cController:
    """An I2C controller on two open-drain lines, as a DUT's would be, at
    `hz`: START, the address, the bytes, a repeated START for a read, STOP.
    Each byte's nine bits are timed as one; the ACK is read from what the
    harness drove on SDA in the middle of the ninth bit's high."""

    def __init__(self, stim: Stimulus, sda: int, scl: int, hz: int = 100_000):
        self.stim, self.sda, self.scl = stim, sda, scl
        self.bit_ns = 1_000_000_000 // hz
        for pin in (sda, scl):
            stim.pull_up(pin)
            stim.watch(pin)
            stim.drive(pin, None, stim.t_ns)

    def _bits(self, start: int, bits) -> int:
        # Each bit: SDA set a quarter in, SCL high from the middle to the
        # end. None leaves SDA released, for the other side to drive.
        t, q = start, self.bit_ns // 4
        for bit in bits:
            self.stim.drive(self.sda, None if bit in (1, None) else 0, t + q)
            self.stim.drive(self.scl, None, t + 2 * q)
            self.stim.drive(self.scl, 0, t + 4 * q)
            t += self.bit_ns
        return t

    def _sample_times(self, start: int, count: int) -> list[int]:
        return [start + i * self.bit_ns + 3 * self.bit_ns // 4 for i in range(count)]

    def _check_stretch(self, start: int, count: int) -> None:
        for i in range(count):
            high = start + i * self.bit_ns + self.bit_ns // 2
            if self.stim.harness_level(self.scl, high + self.bit_ns // 8) == 0:
                raise I2cStretched(f"SCL held low at {high} ns")

    def _start(self, at: int) -> int:
        # From SCL low (a repeated START, or idle): SDA released, then SCL,
        # then SDA falls while SCL is high, then SCL falls; each step a
        # quarter bit apart, past tLOW, tSU;STA and tHD;STA at 100 kHz.
        q = self.bit_ns // 4
        self.stim.drive(self.sda, None, at + q)
        self.stim.drive(self.scl, None, at + 2 * q)
        self.stim.drive(self.sda, 0, at + 3 * q)
        self.stim.drive(self.scl, 0, at + 4 * q)
        return at + 5 * q

    def _stop(self, at: int) -> int:
        q = self.bit_ns // 4
        self.stim.drive(self.sda, 0, at + q)
        self.stim.drive(self.scl, None, at + 2 * q)
        self.stim.drive(self.sda, None, at + 3 * q)
        return at + self.bit_ns

    def _byte_out(self, at: int, byte: int) -> tuple[bool, int]:
        bits = [(byte >> (7 - i)) & 1 for i in range(8)] + [None]
        end = self._bits(at, bits)
        sample = self._sample_times(at, 9)[-1]
        now = self.stim.until(end)
        self._check_stretch(at, 9)
        # The next byte starts no earlier than the hold: SCL stays low
        # between bytes as long as the controller likes.
        return self.stim.line(self.sda, sample) == 0, max(end, now)

    def _byte_in(self, at: int, last: bool) -> tuple[int, int]:
        end = self._bits(at, [None] * 8 + [1 if last else 0])
        now = self.stim.until(end)
        self._check_stretch(at, 9)
        value = 0
        for t in self._sample_times(at, 8):
            value = (value << 1) | self.stim.line(self.sda, t)
        return value, max(end, now)

    def transfer(self, address: int, write: bytes = b"", read: int = 0) -> I2cResult:
        t = self.stim.until(self.stim.t_ns) + self.bit_ns
        data = bytearray()
        if write or not read:
            t = self._start(t)
            acked, t = self._byte_out(t, address << 1)
            for b in write if acked else ():
                acked, t = self._byte_out(t, b)
                if not acked:
                    break
            if not acked or not read:
                self.stim.until(self._stop(t))
                self.stim.go()
                return I2cResult(acked)
        t = self._start(t)
        acked, t = self._byte_out(t, (address << 1) | 1)
        for i in range(read if acked else 0):
            value, t = self._byte_in(t, last=i == read - 1)
            data.append(value)
        self.stim.until(self._stop(t))
        self.stim.go()
        return I2cResult(acked, bytes(data))
