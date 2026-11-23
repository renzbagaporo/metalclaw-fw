# SPDX-License-Identifier: Apache-2.0 OR MIT
# A part a program on the DUT talks to, as a harness model attached before
# the program starts (Attach), and what the part saw of it afterwards
# (Check). A binding names DUT pins; the bench's fixture says which harness
# pin reaches each one. On the carrier a Pico pin can have two harness
# branches, and a PIO engine reaches 32 consecutive GPIOs from base 0 or 16,
# so the pins of one binding come from one window.

import itertools
from dataclasses import dataclass, field

WINDOWS = (range(0, 32), range(16, 48))


# The harness's I2C target is an I2C controller in target mode, so SDA and
# SCL need its pin functions: I2C0 on GPIO 4k / 4k+1, I2C1 on 4k+2 / 4k+3
# (RP2040 and RP2350 GPIO function tables).
def i2c_pins(pins) -> bool:
    sda, scl = pins
    return (sda % 4, scl % 4) in ((0, 1), (2, 3))


# A steps_out wire drives its pins as one PIO OUT group: consecutive GPIOs.
def consecutive_pins(pins) -> bool:
    return all(p == pins[0] + i for i, p in enumerate(pins))


# Per binding name, what its pins must satisfy beyond one PIO window.
RULES = {"i2c": i2c_pins, "ab": consecutive_pins}
# Per pair of single-pin bindings, what their pins must satisfy together:
# an I2C controller's sda and scl, one hardware block's pair.
PAIR_RULES = {("sda", "scl"): i2c_pins}


class PinError(ValueError):
    pass


def harness_pins(fixture: dict, dut_pins, rule=None) -> tuple[int, ...]:
    reach = fixture["dut_pins"]
    missing = [p for p in dut_pins if p not in reach]
    if missing:
        raise PinError(f"no harness pin reaches DUT GP{missing}")
    for window in WINDOWS:
        options = [[b for b in reach[p] if b in window] for p in dut_pins]
        for chosen in itertools.product(*options):
            if len(set(chosen)) == len(chosen) and (rule is None or rule(chosen)):
                return tuple(chosen)
    raise PinError(f"DUT GP{list(dut_pins)} have no harness branches in one 32-pin window"
                   + (f" that suit {rule.__name__}" if rule else ""))


@dataclass(frozen=True)
class Check:
    # What a part saw of the program, read on the harness once the program
    # has run: `expression` is evaluated there with the attached instance as
    # `part` (a property, a region, a stats() field), and `test` judges the
    # value that comes back. A display's frame, an LED strip's colours, the
    # bytes a flash part was given.
    expression: str
    test: object  # callable(value) -> bool
    description: str
    # Bench seconds the program gets before the check, after a restart.
    after_seconds: float = 2.0
    # Run on the harness with the instance as `part` right after the
    # restart: what the part is to do for this run.
    setup: str = ""
    # Run `setup` before the restart instead, with the DUT halted and what
    # its UARTs had queued sent: a baseline the run's own traffic cannot
    # beat, however late the harness gets to it in a loaded emulator.
    before_restart: bool = False


# The W25Q's counters at a restart, once an erase or program in flight has
# ended (the model keeps BUSY for 9.6's typical times, a sector erase 45 ms;
# the wait gives up after 3 s), and what the run did since.
FLASH_AT_RESTART = ("import time\nfor _ in range(600):\n    if not part.busy:\n        break\n    time.sleep(0.005)\n"
                    "flash_at_restart = (part.programs, part.erases, part.lost, part.ignored)\n")
FLASH_SINCE_RESTART = ("tuple(now - then for now, then in zip((part.programs, part.erases, part.lost, part.ignored), "
                       "flash_at_restart))")


@dataclass(frozen=True)
class Attach:
    # metalclaw.attach(model, name, **bindings): each binding a tuple of DUT
    # pins, then any plain values (an I2C address, say) after them.
    model: str
    name: str
    bindings: dict = field(default_factory=dict)
    # How many leading entries of each binding's tuple are pins.
    pin_counts: dict = field(default_factory=dict)
    # Properties set once attached: name -> value.
    properties: dict = field(default_factory=dict)
    checks: tuple = ()
    # An open-drain part with no pull-up of its own: it works only on a
    # bench whose lines have pull-ups (the carrier's 5.1 k), since a
    # harness pin's internal pull-up (50-80 k) cannot outpull the DUT pad's
    # pull-down, which gpio_init leaves on.
    needs_line_pullups: bool = False
    # The boards (example.json "board": pico, pico2) whose build uses this
    # part; empty
    # for every board.
    boards: tuple[str, ...] = ()
    # Every binding's pins from one PIO window together, for a part whose
    # engine links its pins (a jumper); or the names of the bindings whose
    # pins must share one, the rest choosing theirs on their own (a radio
    # whose SPI engine drives BUSY, beside a plain reset line).
    one_window: bool | tuple[str, ...] = False
    # Properties that differ on a bench whose harness cannot hold the
    # part's pins at an analog level (fixture "analog_pins"), only high or
    # low.
    without_analog: dict = field(default_factory=dict)

    def dut_pins(self) -> tuple[int, ...]:
        return tuple(p for b, v in self.bindings.items() for p in v[:self.pin_counts.get(b, len(v))])

    def properties_for(self, fixture: dict) -> dict:
        if set(self.dut_pins()) <= set(fixture.get("analog_pins", ())):
            return self.properties
        return {**self.properties, **self.without_analog}

    def _pins(self, fixture: dict) -> dict:
        counts = {b: self.pin_counts.get(b, len(v)) for b, v in self.bindings.items()}
        linked = tuple(self.bindings) if self.one_window is True else tuple(self.one_window or ())
        pins = {}
        if linked:
            chosen = iter(harness_pins(fixture, tuple(p for b in linked for p in self.bindings[b][:counts[b]])))
            pins = {b: tuple(next(chosen) for _ in range(counts[b])) for b in linked}
        for pair, rule in PAIR_RULES.items():
            if all(b in self.bindings and b not in pins and counts[b] == 1 for b in pair):
                chosen = harness_pins(fixture, tuple(self.bindings[b][0] for b in pair), rule)
                pins.update({b: (p,) for b, p in zip(pair, chosen)})
        for b, v in self.bindings.items():
            if b not in pins:
                pins[b] = harness_pins(fixture, v[:counts[b]], RULES.get(b))
        return pins

    def code(self, fixture: dict) -> str:
        pins = self._pins(fixture)
        args = [f"{b}={tuple(pins[b]) + tuple(v[self.pin_counts.get(b, len(v)):])!r}"
                for b, v in self.bindings.items()]
        lines = [f"{self.name} = metalclaw.attach({self.model!r}, {self.name!r}, {', '.join(args)})"]
        lines += [f"{self.name}.{k} = {v!r}" for k, v in self.properties_for(fixture).items()]
        return "import metalclaw\n" + "\n".join(lines) + "\n"


# A strip's frames once the program is sending again after the check's
# restart. On the carrier the line's pull-up holds DIN high from the reset
# until the program drives it, and that pulse ends one spoiled frame: the
# frame the reset cut, at the first good frame at the latest
# (pio_ws2812_parallel's two strips: bad_frames 0 before the restart, 1 by
# 0.3 s after it, still 1 at 2.7 s). The count starts after two good
# frames; a part that has seen no restart counts from 0.
def ws2812_frames(name: str, length: int, description: str) -> Check:
    return Check(f"(part.frames > 10, part.bad_frames - globals().get('_{name}_bad', 0), part.length)",
                 lambda v: v == (True, 0, length), description, 2.5,
                 setup=f"import time\n_f = part.frames\nwhile part.frames < _f + 2:\n    time.sleep_ms(1)\n"
                       f"_{name}_bad = part.bad_frames\n")


def i2c_jumpers() -> tuple[Attach, ...]:
    # An I2C target on GP4 (SDA) / GP5 (SCL) and a controller on GP6 / GP7,
    # wired SDA to SDA and SCL to SCL; SCL through the clock jumper, which
    # keeps the target's clock stretching.
    return (Attach("jumper_open_drain", "sda", {"a": (4,), "b": (6,)}, one_window=True),
            Attach("jumper_open_drain_clock", "scl", {"a": (5,), "b": (7,)}, one_window=True))
