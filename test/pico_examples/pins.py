# SPDX-License-Identifier: Apache-2.0 OR MIT
# What a pico-examples program does on its pins, read from the DUT itself
# over SWD while it runs: SIO GPIO_IN, and each watched pin's IO_BANK0
# GPIO_STATUS OETOPAD (whatever drives it: SIO, PIO, PWM), stamped with the DUT's
# own 1 MHz timer, so the times are bench time on the bench and in the
# emulator alike. The DUT keeps running (DapMemory never halts it). This
# sees every pin, the board-internal LED (GP25) included, which no harness
# pin reaches. Resolution is one poll, a few milliseconds on the bench.
#
# Register addresses from the pico-sdk's hardware_regs headers
# (addressmap.h, sio.h, timer.h, io_bank0.h, io_qspi.h, usb.h) for each chip.

import time
from dataclasses import dataclass

CHIPS = {
    # TIMER_BASE + TIMERAWL, SIO GPIO_IN, IO_BANK0_BASE (GPIOn_STATUS at 8n)
    "rp2040": (0x40054000 + 0x28, 0xD0000004, 0x40014000),
    "rp2350": (0x400B0000 + 0x28, 0xD0000004, 0x40028000),
}
STATUS_OETOPAD = 1 << 13

# IO_QSPI_BASE + GPIO_QSPI_SS_CTRL, whose INOVER (bits 17:16) set to LOW
# (2) makes every reader of QSPI_SS's input, SIO's GPIO_HI_IN included, see
# it low: the input path the Pico's BOOTSEL button pulls through 1 kOhm.
QSPI_SS_CTRL = {"rp2040": 0x40018000 + 0x0C, "rp2350": 0x40030000 + 0x1C}
INOVER_LOW_BIT = 1 << 17
# The bus fabric's atomic set and clear aliases (addressmap.h
# REG_ALIAS_SET_BITS, REG_ALIAS_CLR_BITS), so a press never races the
# program's own read-modify-write of the same register.
ALIAS_SET, ALIAS_CLR = 0x2000, 0x3000
# USBCTRL_REGS_BASE + MAIN_CTRL (CONTROLLER_EN, bit 0) and + SIE_CTRL
# (PULLUP_EN, bit 16): the same on both chips.
USB_MAIN_CTRL, USB_SIE_CTRL = 0x50110000 + 0x40, 0x50110000 + 0x4C
# Host seconds without the DUT's timer moving before a watch gives up: a
# core halted by debug pauses the timer (TIMER DBGPAUSE), and a program
# that ends in a breakpoint does that.
STALL_S = 5.0


def bootsel(link, chip, pressed: bool) -> None:
    alias = ALIAS_SET if pressed else ALIAS_CLR
    link.write_words(QSPI_SS_CTRL[chip] + alias, [INOVER_LOW_BIT])


# Whether the DUT's USB controller is on with its D+ pull-up: presenting a
# device to a host, as the bootrom's USB boot does at once.
def usb_presenting(link, chip) -> bool:
    main, sie = link.read_words(USB_MAIN_CTRL, 1)[0], link.read_words(USB_SIE_CTRL, 1)[0]
    return bool(main & 1) and bool(sie & (1 << 16))


@dataclass(frozen=True)
class Sample:
    us: int  # DUT TIMERAWL, unwrapped
    levels: int  # GPIO_IN
    driven: int  # OETOPAD of the watched pins


def watch(memory, chip, seconds, pins=(), link=None, actions=()):
    # Samples for `seconds` of DUT time, as fast as the link allows; `driven`
    # carries OETOPAD for `pins` only. Each action (seconds, fn(link)) runs
    # once the DUT's time has reached it.
    timer, gpio_in, io_bank0 = CHIPS[chip]
    samples = []
    pending = sorted(actions, key=lambda a: a[0])
    with (link or memory.link()) as l:
        last = l.read_words(timer, 1)[0]
        unwrapped = 0
        moved = time.monotonic()
        while unwrapped < seconds * 1e6:
            while pending and pending[0][0] * 1e6 <= unwrapped:
                pending.pop(0)[1](l)
            levels = l.read_words(gpio_in, 1)[0]
            driven = 0
            for pin in pins:
                if l.read_words(io_bank0 + 8 * pin, 1)[0] & STATUS_OETOPAD:
                    driven |= 1 << pin
            now = l.read_words(timer, 1)[0]
            if now != last:
                moved = time.monotonic()
            elif time.monotonic() - moved > STALL_S:
                break
            unwrapped += (now - last) % (1 << 32)
            last = now
            samples.append(Sample(unwrapped, levels, driven))
    return samples


def edges(samples, pin):
    # (time in seconds, new level) for each change seen on `pin`.
    out, previous = [], None
    for s in samples:
        level = (s.levels >> pin) & 1
        if previous is not None and level != previous:
            out.append((s.us / 1e6, level))
        previous = level
    return out


def driven_throughout(samples, pin):
    return bool(samples) and all((s.driven >> pin) & 1 for s in samples)


@dataclass(frozen=True)
class Toggle:
    # A pin the program drives and toggles: at least `edges` changes, each
    # interval between them within `period` (min, max) seconds. With
    # `until`, all of that up to that many seconds into the watch, and no
    # change after it.
    pin: int
    edges: int
    period: tuple[float, float]
    until: float | None = None

    def check(self, samples) -> list[str]:
        problems = []
        if self.until is not None:
            late = [t for t, _ in edges(samples, self.pin) if t > self.until]
            if late:
                problems.append(f"GP{self.pin} still changed at {[round(t, 3) for t in late[:8]]} s")
            samples = [s for s in samples if s.us <= self.until * 1e6]
        if not driven_throughout(samples[len(samples) // 4:], self.pin):
            problems.append(f"GP{self.pin} is not driven as an output")
        seen = edges(samples, self.pin)
        if len(seen) < self.edges:
            problems.append(f"GP{self.pin} changed {len(seen)} times, expected at least {self.edges}")
        gaps = [b[0] - a[0] for a, b in zip(seen, seen[1:])]
        bad = [round(g, 4) for g in gaps if not self.period[0] <= g <= self.period[1]]
        if bad:
            problems.append(f"GP{self.pin} intervals outside {self.period} s: {bad[:8]}")
        return problems


def runs(samples, pin):
    # (level, seconds) for each stretch the pin held between two changes;
    # the stretches cut by the watch's ends are left out.
    seen = edges(samples, pin)
    return [(a[1], b[0] - a[0]) for a, b in zip(seen, seen[1:])]


MORSE = {
    ".-": "A", "-...": "B", "-.-.": "C", "-..": "D", ".": "E", "..-.": "F", "--.": "G", "....": "H",
    "..": "I", ".---": "J", "-.-": "K", ".-..": "L", "--": "M", "-.": "N", "---": "O", ".--.": "P",
    "--.-": "Q", ".-.": "R", "...": "S", "-": "T", "..-": "U", "...-": "V", ".--": "W", "-..-": "X",
    "-.--": "Y", "--..": "Z",
}


def morse(samples, pin, unit):
    # The text a pin keys in Morse at `unit` seconds a dot: high one unit
    # for a dot, three for a dash; low one unit between elements, three
    # between letters, seven between words; anything longer ends a message,
    # shown as "|".
    out, letter = [], ""
    for level, length in runs(samples, pin):
        units = length / unit
        if level:
            letter += "." if units < 2 else "-"
            continue
        if units < 2:
            continue
        out.append(MORSE.get(letter, "?") if letter else "")
        letter = ""
        if units >= 9:
            out.append("|")
        elif units >= 5:
            out.append(" ")
    return "".join(out)


@dataclass(frozen=True)
class Morse:
    # A pin the program keys `text` on, whole, between two pauses.
    pin: int
    text: str
    unit: float

    def check(self, samples) -> list[str]:
        problems = []
        if not driven_throughout(samples[len(samples) // 4:], self.pin):
            problems.append(f"GP{self.pin} is not driven as an output")
        got = morse(samples, self.pin, self.unit)
        if f"|{self.text}|" not in got:
            problems.append(f"GP{self.pin} keyed {got!r}, not {self.text!r} between two pauses")
        return problems


# Seven-segment patterns, segment A in bit 0 to G in bit 6, for 0..9: the
# common table, the one gpio/hello_7segment/hello_7segment.c:32-43 uses.
SEGMENT_DIGITS = (0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x67)


def digits(samples, first, lit_low=True):
    # (time, digit) each time the seven pins from `first` up settle on
    # another digit; patterns that are no digit (a blank between two) are
    # skipped.
    out = []
    for s in samples:
        pattern = (s.levels >> first) & 0x7F
        if lit_low:
            pattern ^= 0x7F
        if pattern in SEGMENT_DIGITS:
            d = SEGMENT_DIGITS.index(pattern)
            if not out or out[-1][1] != d:
                out.append((s.us / 1e6, d))
    return out


@dataclass(frozen=True)
class Segments:
    # A seven-segment count on the pins from `first`: each change one digit
    # up while the `button` pin is low (pressed) and one down while it is
    # high, at least `steps` changes and `each_way` in both directions.
    # Changes within `settle` seconds of a button edge may go either way.
    first: int
    button: int
    steps: int
    each_way: int
    settle: float = 0.3
    lit_low: bool = True

    @property
    def pin(self) -> int:
        return self.first

    def check(self, samples) -> list[str]:
        problems = []
        shown = digits(samples, self.first, self.lit_low)
        presses = [t for t, _ in edges(samples, self.button)]
        ups = downs = 0
        for (t0, a), (t1, b) in zip(shown, shown[1:]):
            step = (b - a) % 10
            if step not in (1, 9):
                problems.append(f"the display went from {a} to {b} at {t1:.3f} s")
                continue
            if any(abs(t1 - t) < self.settle for t in presses):
                continue
            before = [s for s in samples if s.us / 1e6 < t1]
            pressed = not (before[-1].levels >> self.button) & 1 if before else False
            if (step == 1) != pressed:
                problems.append(f"the display went from {a} to {b} at {t1:.3f} s with GP{self.button} "
                                f"{'low' if pressed else 'high'}")
            ups += step == 1
            downs += step == 9
        if len(shown) - 1 < self.steps:
            problems.append(f"the display changed {len(shown) - 1} times, expected at least {self.steps}")
        if min(ups, downs) < self.each_way:
            problems.append(f"the display counted up {ups} and down {downs} times, expected {self.each_way} each")
        return problems


@dataclass(frozen=True)
class Levels:
    # A pin that holds `level` through each (start, end) window of the
    # watch, `margin` seconds in from either end.
    pin: int
    windows: tuple[tuple[float, float, int], ...]
    margin: float = 0.1

    def check(self, samples) -> list[str]:
        problems = []
        for start, end, level in self.windows:
            inside = [s for s in samples if start + self.margin <= s.us / 1e6 <= end - self.margin]
            wrong = [s for s in inside if (s.levels >> self.pin) & 1 != level]
            if not inside:
                problems.append(f"no samples of GP{self.pin} between {start} and {end} s")
            elif wrong:
                problems.append(f"GP{self.pin} was not {level} between {start} and {end} s "
                                f"({len(wrong)} of {len(inside)} samples)")
        return problems
