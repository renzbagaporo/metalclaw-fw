# SPDX-License-Identifier: Apache-2.0 OR MIT
# A PIO emulator for host tests: state machines running encoded programs on
# their own clocks, against shared lines, one instruction or delay cycle per
# clock, from the RP2040 datasheet's description (3.4, 3.5): side-set at
# issue, stalls on WAIT, a full or empty FIFO, delays after an instruction
# completes, autopush and autopull at their thresholds, and the two
# system-clock cycles of the input synchronizer on every pin read.
#
# A line is pulled high, pulled low or held at its last level when nothing
# drives it; when several drivers do, the line is low if any drives it low
# (open drain). The assembler takes the pioasm subset the pico-examples
# programs these tests run use.

import bisect
import heapq
import re
from collections import deque

PS = 1
NS = 1000 * PS
US = 1000 * NS


def period_ps(hz: float, div: float = 1.0) -> int:
    return round(1e12 * div / hz)


class Line:
    def __init__(self, pull=None):
        self.pull = pull
        self.drivers = {}
        self.times = [0]
        self.levels = [1 if pull == "up" else 0]

    def level(self) -> int:
        driven = [v for (on, v) in self.drivers.values() if on]
        if driven:
            return min(driven)
        if self.pull == "up":
            return 1
        if self.pull == "down":
            return 0
        return self.levels[-1]

    def at(self, t: int) -> int:
        return self.levels[bisect.bisect_right(self.times, t) - 1]

    def update(self, t: int):
        new = self.level()
        if new != self.levels[-1]:
            if self.times[-1] == t:
                self.times.pop()
                self.levels.pop()
                if self.levels and self.levels[-1] == new:
                    return
            self.times.append(t)
            self.levels.append(new)

    def edges(self):
        return list(zip(self.times, self.levels))


class Bus:
    def __init__(self, pins=48):
        self.lines = [Line() for _ in range(pins)]

    def pull(self, pin, how):
        self.lines[pin] = Line(how)

    def drive(self, source, pin, on, value, t):
        line = self.lines[pin]
        line.drivers[source] = (bool(on), 1 if value else 0)
        line.update(t)

    def at(self, pin, t):
        return self.lines[pin].at(t)


def _bits(word, lo, n):
    return (word >> lo) & ((1 << n) - 1)


def _reverse(v):
    return int(f"{v & 0xffffffff:032b}"[::-1], 2)


M32 = 0xffffffff


class StateMachine:
    def __init__(self, sim, name, program, *, hz, div=1.0, sys_hz=None, offset=0, wrap=None, wrap_target=0,
                 in_base=0, out_base=0, out_count=1, set_base=0, set_count=1, sideset_base=0, sideset_bits=0,
                 sideset_opt=False, sideset_pindirs=False, jmp_pin=0, in_right=False, autopush=False, push_at=32,
                 out_right=True, autopull=False, pull_at=32, join=None, status_sel=0, status_n=0, block=None):
        self.sim = sim
        self.name = name
        self.mem = list(program)
        self.offset = offset
        self.period = period_ps(hz, div)
        self.sync = 2 * period_ps(sys_hz or hz)
        self.wrap_top = offset + (len(program) - 1 if wrap is None else wrap)
        self.wrap_target = offset + wrap_target
        self.in_base, self.out_base, self.out_count = in_base, out_base, out_count
        self.set_base, self.set_count = set_base, set_count
        self.ss_base, self.ss_bits, self.ss_opt, self.ss_pindirs = sideset_base, sideset_bits, sideset_opt, sideset_pindirs
        self.jmp_pin = jmp_pin
        self.in_right, self.autopush, self.push_at = in_right, autopush, push_at
        self.out_right, self.autopull, self.pull_at = out_right, autopull, pull_at
        self.status_sel, self.status_n = status_sel, status_n
        self.rx_depth = 8 if join == "rx" else 0 if join == "tx" else 4
        self.tx_depth = 8 if join == "tx" else 0 if join == "rx" else 4
        self.rx, self.tx = deque(), deque()
        self.block = block if block is not None else sim.default_block
        self.pc = offset
        self.x = self.y = self.isr = self.osr = 0
        self.isr_count = 0
        self.osr_count = 32
        self.delay = 0
        self.enabled = True
        self.next_t = 0
        self.values = {}
        self.dirs = {}
        self.pending_exec = None
        self.stalls = 0
        # Set when a PULL or an autopull OUT stalls on an empty TX FIFO, as
        # FDEBUG's TXSTALL is; the CPU clears it.
        self.tx_stalled = False
        sim.sms.append(self)

    # --- pins
    def read(self, pin, t):
        return self.sim.bus.at(pin % len(self.sim.bus.lines), t - self.sync)

    def in_pin(self, index):
        # The IN mapping wraps within the 32 pins the block reaches.
        return (self.in_base + index) % 32

    def read_in(self, t):
        v = 0
        for i in range(32):
            v |= self.read(self.in_pin(i), t) << i
        return v

    def _apply(self, pin, t):
        self.sim.bus.drive(self.name, pin, self.dirs.get(pin, 0), self.values.get(pin, 0), t)

    def set_pins(self, base, count, data, t, dirs=False):
        for i in range(count):
            pin = base + i
            if dirs:
                self.dirs[pin] = (data >> i) & 1
            else:
                self.values[pin] = (data >> i) & 1
            self._apply(pin, t)

    def init_pins(self, values=None, dirs=None, t=0):
        for pin, v in (values or {}).items():
            self.values[pin] = v
        for pin, d in (dirs or {}).items():
            self.dirs[pin] = d
        for pin in set(self.values) | set(self.dirs):
            self._apply(pin, t)

    # --- CPU side
    def put(self, word):
        assert len(self.tx) < max(self.tx_depth, 1), f"{self.name}: TX FIFO full"
        self.tx.append(word & M32)

    def tx_full(self):
        return len(self.tx) >= self.tx_depth

    def get(self):
        return self.rx.popleft()

    def exec(self, word):
        self.pending_exec = word

    def restart(self, pc=None):
        self.pc = self.offset if pc is None else pc
        self.isr = self.isr_count = 0
        self.osr_count = 32
        self.delay = 0

    # --- execution
    def _sideset(self, word, t):
        field = _bits(word, 8, 5)
        if self.ss_bits == 0:
            return field
        delay_bits = 5 - self.ss_bits
        delay = field & ((1 << delay_bits) - 1)
        side = field >> delay_bits
        if self.ss_opt:
            if not side >> (self.ss_bits - 1):
                return delay
            side &= (1 << (self.ss_bits - 1)) - 1
            count = self.ss_bits - 1
        else:
            count = self.ss_bits
        self.set_pins(self.ss_base, count, side, t, dirs=self.ss_pindirs)
        return delay

    def _status(self):
        level = len(self.rx) if self.status_sel else len(self.tx)
        return M32 if level < self.status_n else 0

    def _src(self, code, t):
        return {0: lambda: self.read_in(t), 1: lambda: self.x, 2: lambda: self.y, 3: lambda: 0,
                5: self._status, 6: lambda: self.isr, 7: lambda: self.osr}[code]()

    def _push(self, block):
        if len(self.rx) >= self.rx_depth:
            if block:
                return False
        else:
            self.rx.append(self.isr & M32)
        self.isr = 0
        self.isr_count = 0
        return True

    def _irq_index(self, index):
        if index & 0x10:
            return (index & 0x4) | ((index + self.sim.sms.index(self) % 4) & 3)
        return index & 7

    def step(self, t):
        if not self.enabled:
            return
        if self.pending_exec is not None:
            word = self.pending_exec
            self.pending_exec = None
            self.delay = 0
            self._execute(word, t)
            return
        if self.delay:
            self.delay -= 1
            return
        word = self.mem[self.pc - self.offset]
        done, jumped = self._execute(word, t)
        if not done:
            self.stalls += 1
            return
        if not jumped:
            self.pc = self.wrap_target if self.pc == self.wrap_top else self.pc + 1

    def _execute(self, word, t):
        delay = self._sideset(word, t)
        op = word >> 13
        a, b = _bits(word, 5, 3), _bits(word, 0, 5)
        jumped = False
        if op == 0:
            cond = a
            take = {0: True, 1: self.x == 0, 2: self.x != 0, 3: self.y == 0, 4: self.y != 0,
                    5: self.x != self.y, 6: self.read(self.jmp_pin, t) == 1,
                    7: self.osr_count < self.pull_at}[cond]
            if cond == 2:
                self.x = (self.x - 1) & M32
            if cond == 4:
                self.y = (self.y - 1) & M32
            if take:
                self.pc = self.offset + b
                jumped = True
        elif op == 1:
            pol, src, idx = word >> 7 & 1, a & 3, b
            if src == 0:
                ok = self.read(idx, t) == pol
            elif src == 1:
                ok = self.read(self.in_pin(idx), t) == pol
            else:
                n = self._irq_index(idx)
                ok = self.block.irq[n] == pol
                if ok and pol:
                    self.block.irq[n] = 0
            if not ok:
                return False, False
        elif op == 2:
            n = b or 32
            if self.autopush and self.isr_count >= self.push_at and len(self.rx) >= self.rx_depth:
                return False, False
            data = self._src(a, t) & ((1 << n) - 1)
            if self.in_right:
                self.isr = ((self.isr >> n) | (data << (32 - n))) & M32 if n < 32 else data
            else:
                self.isr = ((self.isr << n) | data) & M32 if n < 32 else data
            self.isr_count = min(32, self.isr_count + n)
            if self.autopush and self.isr_count >= self.push_at:
                self._push(True)
        elif op == 3:
            n = b or 32
            if self.autopull and self.osr_count >= self.pull_at:
                if not self.tx:
                    self.tx_stalled = True
                    return False, False
                self.osr = self.tx.popleft()
                self.osr_count = 0
            mask = (1 << n) - 1
            if self.out_right:
                data = self.osr & mask
                self.osr = (self.osr >> n) if n < 32 else 0
            else:
                data = (self.osr >> (32 - n)) & mask
                self.osr = (self.osr << n) & M32 if n < 32 else 0
            self.osr_count = min(32, self.osr_count + n)
            if a == 0:
                self.set_pins(self.out_base, self.out_count, data, t)
            elif a == 1:
                self.x = data
            elif a == 2:
                self.y = data
            elif a == 4:
                self.set_pins(self.out_base, self.out_count, data, t, dirs=True)
            elif a == 5:
                self.pc = self.offset + data
                jumped = True
            elif a == 6:
                self.isr = data
                self.isr_count = n
        elif op == 4:
            if not word & 0x80:
                if word & 0x40 and self.isr_count < self.push_at:
                    pass
                elif not self._push(bool(word & 0x20)):
                    return False, False
            else:
                if word & 0x40 and self.osr_count < self.pull_at:
                    pass
                elif self.tx:
                    self.osr = self.tx.popleft()
                    self.osr_count = 0
                elif word & 0x20:
                    self.tx_stalled = True
                    return False, False
                else:
                    self.osr = self.x
                    self.osr_count = 0
        elif op == 5:
            dest, mop, src = a, _bits(word, 3, 2), _bits(word, 0, 3)
            v = self._src(src, t)
            if mop == 1:
                v = ~v & M32
            elif mop == 2:
                v = _reverse(v)
            if dest == 0:
                self.set_pins(self.out_base, self.out_count, v, t)
            elif dest == 1:
                self.x = v
            elif dest == 2:
                self.y = v
            elif dest == 5:
                self.pc = self.offset + (v & 31)
                jumped = True
            elif dest == 6:
                self.isr = v
                self.isr_count = 0
            elif dest == 7:
                self.osr = v
                self.osr_count = 0
        elif op == 6:
            clear, wait, n = word >> 6 & 1, word >> 5 & 1, self._irq_index(b)
            if clear:
                self.block.irq[n] = 0
            else:
                self.block.irq[n] = 1
                if wait:
                    raise NotImplementedError("irq wait")
        elif op == 7:
            if a == 0:
                self.set_pins(self.set_base, self.set_count, b, t)
            elif a == 1:
                self.x = b
            elif a == 2:
                self.y = b
            elif a == 4:
                self.set_pins(self.set_base, self.set_count, b, t, dirs=True)
        self.delay = delay
        return True, jumped


class Block:
    def __init__(self):
        self.irq = [0] * 8


class Sim:
    def __init__(self, pins=48):
        self.bus = Bus(pins)
        self.sms = []
        self.t = 0
        self.events = []
        self.serial = 0
        self.default_block = Block()

    def at(self, t, fn):
        self.serial += 1
        heapq.heappush(self.events, (t, self.serial, fn))

    def drive(self, t, source, pin, on, value=0):
        self.at(t, lambda now: self.bus.drive(source, pin, on, value, now))

    def waveform(self, source, pin, edges, push_pull=True):
        # edges: (t, level); a push-pull driver holds each level until the
        # next, an open-drain one only pulls low.
        for t, level in edges:
            if push_pull:
                self.drive(t, source, pin, True, level)
            else:
                self.drive(t, source, pin, not level, 0)

    def run_until(self, end, until=None):
        while True:
            if until is not None and until():
                return True
            nxt = min((sm.next_t for sm in self.sms if sm.enabled), default=end)
            if self.events and self.events[0][0] <= nxt:
                if self.events[0][0] > end:
                    self.t = end
                    return False
                t, _, fn = heapq.heappop(self.events)
                self.t = max(self.t, t)
                fn(self.t)
                continue
            if nxt > end:
                self.t = end
                return False
            self.t = nxt
            for sm in self.sms:
                if sm.enabled and sm.next_t == nxt:
                    sm.step(nxt)
                    sm.next_t = nxt + sm.period

    def run_for(self, duration, until=None):
        return self.run_until(self.t + duration, until)


# --- a small pioasm

_JMP = {"": 0, "!x": 1, "x--": 2, "!y": 3, "y--": 4, "x!=y": 5, "pin": 6, "!osre": 7}
_SRC = {"pins": 0, "x": 1, "y": 2, "null": 3, "status": 5, "isr": 6, "osr": 7}
_DST = {"pins": 0, "x": 1, "y": 2, "null": 3, "pindirs": 4, "pc": 5, "isr": 6, "osr": 7, "exec": 4}


def assemble(text: str, defines=None):
    # Returns (words, labels, wrap_target, wrap, sideset bits incl. opt, opt, pindirs).
    defines = dict(defines or {})
    lines = []
    ss_bits, ss_opt, ss_dirs = 0, False, False
    wrap_target, wrap = None, None
    labels = {}
    for raw in text.splitlines():
        line = re.split(r"[;]|//", raw)[0].strip()
        if not line:
            continue
        if line.startswith(".define"):
            parts = line.split()
            name, value = parts[-2], parts[-1]
            defines[name] = int(eval(value, {}, defines))
            continue
        if line.startswith(".side_set"):
            parts = line.split()
            ss_bits = int(parts[1])
            ss_opt = "opt" in parts
            ss_dirs = "pindirs" in parts
            if ss_opt:
                ss_bits += 1
            continue
        if line.startswith(".wrap_target"):
            wrap_target = len(lines)
            continue
        if line.startswith(".wrap"):
            wrap = len(lines) - 1
            continue
        if line.startswith("."):
            continue
        m = re.match(r"^(?:public\s+|PUBLIC\s+)?([A-Za-z_]\w*):\s*(.*)$", line)
        if m:
            labels[m.group(1)] = len(lines)
            line = m.group(2).strip()
            if not line:
                continue
        lines.append(line)

    def value(expr):
        expr = expr.strip()
        if expr in labels:
            return labels[expr]
        return int(eval(expr.replace("::", ""), {}, defines))

    words = []
    for line in lines:
        delay, side = 0, None
        m = re.search(r"\[([^\]]+)\]", line)
        if m:
            delay = value(m.group(1))
            line = (line[:m.start()] + " " + line[m.end():]).strip()
        m = re.search(r"\bside\s+(\S+)\s*$", line)
        if m:
            side = value(m.group(1))
            line = line[:m.start()].strip()
        op, _, args = line.partition(" ")
        op = op.lower()
        args = [a.strip() for a in args.split(",")] if args.strip() else []
        lower = [a.lower() for a in args]
        if op == "jmp":
            words_ = " ".join(args).replace(",", " ").split()
            cond = words_[0].lower() if len(words_) == 2 else ""
            word = 0x0000 | _JMP[cond] << 5 | value(words_[-1])
        elif op == "wait":
            tokens = " ".join(args).replace(",", " ").split()
            pol, src, idx = int(tokens[0]), tokens[1].lower(), tokens[2]
            word = 0x2000 | pol << 7 | {"gpio": 0, "pin": 1, "irq": 2}[src] << 5 | value(idx)
        elif op == "in":
            word = 0x4000 | _SRC[lower[0]] << 5 | (value(args[1]) & 31)
        elif op == "out":
            word = 0x6000 | _DST[lower[0]] << 5 | (value(args[1]) & 31)
        elif op == "push":
            word = 0x8000 | ("iffull" in lower) << 6 | ("noblock" not in lower) << 5
        elif op == "pull":
            word = 0x8080 | ("ifempty" in lower) << 6 | ("noblock" not in lower) << 5
        elif op == "mov":
            src = lower[1]
            mop = 0
            if src.startswith("!") or src.startswith("~"):
                mop, src = 1, src[1:]
            elif src.startswith("::"):
                mop, src = 2, src[2:]
            word = 0xa000 | _DST[lower[0]] << 5 | mop << 3 | _SRC[src.strip()]
        elif op == "set":
            word = 0xe000 | _DST[lower[0]] << 5 | value(args[1])
        elif op == "nop":
            word = 0xa042
        elif op == "irq":
            rest = args[0].split()
            if rest[0].lower() in ("set", "nowait", "wait", "clear"):
                mode, idx = rest[0].lower(), rest[1]
            else:
                mode, idx = "set", rest[0]
            word = 0xc000 | (mode == "clear") << 6 | (mode == "wait") << 5 | value(idx)
        else:
            raise ValueError(f"unknown instruction {line!r}")
        field = delay
        if ss_bits:
            if ss_opt:
                if side is not None:
                    field = 1 << 4 | side << (5 - ss_bits) | delay
            else:
                field = (side or 0) << (5 - ss_bits) | delay
        words.append(word | field << 8)
    return words, labels, wrap_target or 0, (len(words) - 1 if wrap is None else wrap), ss_bits, ss_opt, ss_dirs
