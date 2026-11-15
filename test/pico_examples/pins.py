# SPDX-License-Identifier: Apache-2.0 OR MIT
# What a pico-examples program does on its pins, read from the DUT itself
# over SWD while it runs: SIO GPIO_IN and GPIO_OE, stamped with the DUT's
# own 1 MHz timer, so the times are bench time on the bench and in the
# emulator alike. The DUT keeps running (DapMemory never halts it). This
# sees every pin, the board-internal LED (GP25) included, which no harness
# pin reaches. Resolution is one poll, a few milliseconds on the bench.
#
# Register addresses from the pico-sdk's hardware_regs headers
# (addressmap.h, sio.h, timer.h) for each chip.

from dataclasses import dataclass

CHIPS = {
    # TIMER_BASE + TIMERAWL, SIO GPIO_IN, SIO GPIO_OE
    "rp2040": (0x40054000 + 0x28, 0xD0000004, 0xD0000020),
    "rp2350": (0x400B0000 + 0x28, 0xD0000004, 0xD0000030),
}


@dataclass(frozen=True)
class Sample:
    us: int  # DUT TIMERAWL, unwrapped
    levels: int  # GPIO_IN
    driven: int  # GPIO_OE


def watch(memory, chip, seconds, link=None):
    # Samples for `seconds` of DUT time, as fast as the link allows.
    timer, gpio_in, gpio_oe = CHIPS[chip]
    samples = []
    with (link or memory.link()) as l:
        start = last = l.read_words(timer, 1)[0]
        unwrapped = 0
        while unwrapped < seconds * 1e6:
            levels = l.read_words(gpio_in, 1)[0]
            driven = l.read_words(gpio_oe, 1)[0]
            now = l.read_words(timer, 1)[0]
            unwrapped += (now - last) % (1 << 32)
            last = now
            samples.append(Sample(unwrapped, levels, driven))
    del start
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
    # interval between them within `period` (min, max) seconds.
    pin: int
    edges: int
    period: tuple[float, float]

    def check(self, samples) -> list[str]:
        problems = []
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
