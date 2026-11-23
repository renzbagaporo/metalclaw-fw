# SPDX-License-Identifier: Apache-2.0 OR MIT
# An upstream Zephyr sample on the DUT, driving Metalclaw's own device models,
# judged by its console against its own sample.yaml test (targets.py) and by
# what its parts saw of it (parts.py). The harness bridges the DUT's console
# UART (fixture "uart") to a serial port on this host, the sample restarts
# from reset with the port open, and the case reads until every expected
# line is in or the sample's time is up on the harness's clock.

import ast
import contextlib
import os
import re
import time

import pytest
import serial

from test.zephyr_samples.attach import PinError
from test.zephyr_samples.parts import parts_for
from test.zephyr_samples.spec import console_lines
from test.zephyr_samples.targets import zephyr_spec

NAME = "sample_console"

INTERFACE = "Metalclaw DUT console"
BAUD = 115200

DETACH = f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
"""


# The sample's external parts, attached on the harness before any case
# restarts it, and gone once its cases are done. A part on a DUT pin no
# harness pin reaches on this bench, or one that needs a pull-up this
# bench's lines lack, leaves the sample without
# its outside world, so none of its cases can judge it there.
@pytest.fixture(scope="module")
def parts(harness, sample, fixture):
    attached = parts_for(sample.target, sample.metadata.get("board"))
    for part in attached:
        if part.needs_line_pullups and not fixture["line_pullups"]:
            pytest.skip(f"{sample.target}'s {part.model} needs a pull-up on its line, and this bench has none")
    try:
        codes = [part.code(fixture) for part in attached]
    except PinError as error:
        pytest.skip(f"{sample.target} needs a pin this bench does not reach: {error}")
    for code in codes:
        harness.run(code)
    yield attached
    for part in attached:
        harness.run(f"import metalclaw\nfor i in metalclaw.instances():\n    if i.name == {part.name!r}: i.detach()\n")


# The DUT's console, bridged to a serial port on this host for the module's
# cases.
@pytest.fixture(scope="module")
def console(harness, sample, fixture, parts):
    uart = fixture["uart"]
    harness.run(DETACH + f"metalclaw.attach('console_bridge', {NAME!r}, uart={(uart['tx'], uart['rx'])})\n")
    harness.wait_interface(INTERFACE)
    yield harness.serial_port(INTERFACE)
    harness.run(DETACH)
    harness.wait_interface(INTERFACE, present=False)


# The clock is a REPL round trip, so it is read while the port idles, and
# otherwise at most every CLOCK_EVERY_S of host time: a sample that prints
# without pause still ends on its deadline.
CLOCK_EVERY_S = 0.5

# A file the console is appended to as it arrives, so that a session killed
# before its report still leaves what the sample printed.
CONSOLE_LOG_ENV = "METALCLAW_CONSOLE_LOG"


def console_log():
    path = os.environ.get(CONSOLE_LOG_ENV)
    return open(path, "ab", buffering=0) if path else contextlib.nullcontext()


# Every console read gets at least this many bench seconds: a read ends as
# soon as everything matched, so a long window costs time only on a failure,
# while a short one fails a sample whose loop runs slower than its source's
# own delays suggest.
MIN_SECONDS = 10.0


class Typist:
    # Sends each of a Spec's inputs once its pattern has come in, after where
    # the one before it matched: bytes to the console, a str to the harness
    # as code, for a part that is to act then. With `lines`, a pattern
    # matches one line at a time, as the Spec's own (Spec.lines), the
    # unfinished last line (a prompt) too.
    def __init__(self, port, inputs, harness=None, lines=False):
        self.port = port
        self.harness = harness
        self.inputs = list(inputs)
        self.lines = lines
        self.at = 0

    def _match_end(self, pattern: str, text: str) -> int | None:
        rx = re.compile(pattern)
        if self.lines:
            return next((end for end, line in console_lines(text, self.at) if rx.search(line)), None)
        m = rx.search(text, self.at)
        return None if m is None else m.end()

    def __call__(self, text: str) -> None:
        while self.inputs:
            pattern, data = self.inputs[0]
            end = self._match_end(pattern, text)
            if end is None:
                return
            if isinstance(data, str):
                self.harness.run(data)
            else:
                self.port.write(data)
                self.port.flush()
            self.at = end
            self.inputs.pop(0)


# Until `patterns` (the Spec's console unless given) all matched, or the
# Spec's time is up; `inputs` (its own unless given) are typed as they come
# due. With `arrivals`, when each chunk came in on the bench's clock, for
# Spec.timing: (characters read by then, a reading taken before a read that
# found nothing ahead of the chunk, the first reading after it). A chunk
# that a read returns came in after any earlier read that found nothing
# had returned, and before the next clock reading; the bridge's own delay
# is taken as the same for every line.
def read_console(harness, port, spec, patterns=None, inputs=None, arrivals=None) -> str:
    typist = Typist(port, spec.input if inputs is None else inputs, harness, lines=spec.lines)
    with console_log() as log:
        return _read_console(harness, port, spec, patterns, typist, arrivals, log)


def _read_console(harness, port, spec, patterns, typist, arrivals, log) -> str:
    got = bytearray()
    # An input whose pattern matches nothing yet goes in at once: a sample
    # that prints nothing before it reads (a UART echo on the console pins).
    typist("")
    last = harness.clock()
    deadline = last + max(spec.seconds, MIN_SECONDS)
    quiet, pending = last, []
    checked = time.monotonic()

    def clock():
        nonlocal last, pending
        last = harness.clock()
        if arrivals is not None:
            arrivals.extend((end, lo, last) for end, lo in pending)
        pending = []
        return last
    while True:
        before = last
        chunk = port.read(4096)
        got += chunk
        if chunk:
            if log is not None:
                log.write(chunk)
            pending.append((len(got.decode(errors="replace")), quiet))
            typist(got.decode(errors="replace"))
        else:
            quiet = before
        if chunk and not spec.match(got.decode(errors="replace"), patterns)[0]:
            break
        if not chunk or time.monotonic() - checked >= CLOCK_EVERY_S:
            checked = time.monotonic()
            if clock() >= deadline:
                break
    if pending:
        clock()
    return got.decode(errors="replace")


def test_the_sample_completes(harness, sample, fixture, console, record_property, request):
    spec = zephyr_spec(sample.metadata)
    if spec is None:
        pytest.skip(f"no completion criteria for {sample.target} yet")
    board = sample.metadata.get("board")
    judged = spec.for_board(board)
    if judged is None:
        pytest.skip(spec.why_not(sample.target, board))
    if judged.known_failure:
        request.node.add_marker(pytest.mark.xfail(reason=judged.known_failure, strict=True))
    spec = judged.for_bench(fixture["line_pullups"], fixture.get("analog_pins", ()))
    # An empty read shows nothing: a sample that never ran prints nothing
    # too.
    if not spec.console:
        pytest.skip(f"{sample.target} prints nothing on its console; its parts or pins judge it")
    arrivals = []
    with serial.Serial(console, BAUD, timeout=0.1, exclusive=True) as port:
        port.reset_input_buffer()
        sample.restart()
        text = read_console(harness, port, spec, arrivals=arrivals)
    missing, forbidden = spec.match(text)
    if missing or forbidden:
        # The whole console, for the junit report: the assertion keeps only
        # its end.
        record_property("console", text)
    assert not forbidden, f"{sample.target} printed {forbidden}:\n{text[-2000:]}"
    assert not missing, (f"{sample.target} never printed {missing} within {max(spec.seconds, MIN_SECONDS)} s:"
                         f"\n{text[-2000:]}")
    late = spec.timing(text, arrivals)
    assert not late, f"{sample.target}: {late}:\n{text[-2000:]}"


# The buses whose engine counters a failed part check reports.
COUNTED_BUSES = ("spi", "i2c", "uart")

# The harness's two PL011s as a failed UART part check leaves them: per
# block FR, RIS, MIS and IMSC, then DR when FR.RXFE says a byte is waiting
# (reading it takes the byte). RP2040 UART0/1 at 0x40034000/0x40038000,
# RP2350 at 0x40070000/0x40078000.
HARNESS_UARTS = """import machine, sys
bases = (0x40070000, 0x40078000) if 'RP2350' in sys.implementation._machine else (0x40034000, 0x40038000)
for n, b in enumerate(bases):
    fr = machine.mem32[b + 0x18]
    dr = machine.mem32[b] if not fr & 0x10 else None
    print('uart%d fr=%#x ris=%#x mis=%#x imsc=%#x dr=%s' % (n, fr, machine.mem32[b + 0x3c], machine.mem32[b + 0x40],
          machine.mem32[b + 0x38], 'empty' if dr is None else '%#x' % dr))
"""
# And its PIO blocks, where a UART on a pin pair no PL011 has runs
# (core/ports/rp2/hal/uart_pio.c), with the DMA channels that read their RX
# FIFOs into the receive rings: per block FSTAT, FDEBUG, FLEVEL and IRQ and
# each state machine's ADDR; per such channel READ_ADDR, WRITE_ADDR,
# TRANS_COUNT and CTRL_TRIG. PIO0 and PIO1 at 0x50200000 and 0x50300000, the
# RP2350's PIO2 at 0x50400000; DMA at 0x50000000, 12 channels on the
# RP2040 and 16 on the RP2350, 0x40 apart.
HARNESS_PIOS = """import machine, sys
rp2350 = 'RP2350' in sys.implementation._machine
bases = (0x50200000, 0x50300000) + ((0x50400000,) if rp2350 else ())
for n, b in enumerate(bases):
    print('pio%d fstat=%#x fdebug=%#x flevel=%#x irq=%#x addr=%s' % (n, machine.mem32[b + 0x4], machine.mem32[b + 0x8],
          machine.mem32[b + 0xc], machine.mem32[b + 0x30], [machine.mem32[b + 0xd4 + 0x18 * sm] for sm in range(4)]))
for ch in range(16 if rp2350 else 12):
    c = 0x50000000 + 0x40 * ch
    read = machine.mem32[c]
    if any(b + 0x20 <= read < b + 0x30 for b in bases):
        print('dma%d read=%#x write=%#x count=%d ctrl=%#x' % (ch, read, machine.mem32[c + 0x4], machine.mem32[c + 0x8],
              machine.mem32[c + 0xc]))
"""


# 32 bytes of 10 bits at 9600 baud, rounded up.
UART_DRAIN_S = 0.05
WORKER_STATS = ("import metalclaw\n"
                "print(repr(metalclaw.worker_stats({reset}) if hasattr(metalclaw, 'worker_stats') else None))\n")


def test_its_parts_saw_the_sample(harness, sample, parts, record_property):
    checks = [(part, check) for part in parts for check in part.checks]
    if not checks:
        pytest.skip(f"no part checks for {sample.target}")
    # The harness worker's timing over the judged run, where the firmware
    # has it (metalclaw.worker_stats): reset here, recorded below.
    harness.run(WORKER_STATS.format(reset=True))
    early = [(part, check) for part, check in checks if check.setup and check.before_restart]
    if early:
        # Halted, the sample queues nothing more; what it had queued
        # leaves within a 32-byte FIFO at 9600 baud.
        sample.halt_awake(None, sample.ASLEEP_MAX_S)
        harness.sleep(UART_DRAIN_S)
        for part, check in early:
            harness.run(f"part = {part.name}\n{check.setup}\n")
    sample.restart()
    for part, check in checks:
        if check.setup and not check.before_restart:
            harness.run(f"part = {part.name}\n{check.setup}\n")
    harness.sleep(max(check.after_seconds for _, check in checks))
    record_property("harness_worker_stats", harness.run(WORKER_STATS.format(reset=False)).strip())
    problems = []
    failed = {}
    for n, (part, check) in enumerate(checks):
        value = ast.literal_eval(harness.run(f"part = {part.name}\nprint(repr({check.expression}))\n").strip())
        if not check.test(value):
            problems.append(f"{part.name}: {check.description} (got {value!r:.200})")
            # The whole value, for the junit report: the message keeps 200
            # characters of it.
            record_property(f"part_check_{n}_{part.name}", repr(value))
            failed[part.name] = part
    for name, part in failed.items():
        for bus in COUNTED_BUSES:
            if bus in part.bindings:
                record_property(f"part_{name}_{bus}_stats",
                                harness.run(f"print(repr({name}.stats('{bus}')))\n").strip())
        # A W25Q's last ignored instruction and why (devices/w25q), where
        # the firmware's model has it.
        record_property(f"part_{name}_last_ignored", harness.run(
            f"print(repr(getattr({name}, 'last_ignored', None)))\n").strip())
        # The harness's own registers: an rp2 harness's, which the sim
        # platform's host harness has none of.
        if "uart" in part.bindings and harness.board() != "sim":
            record_property("harness_uart_registers", harness.run(HARNESS_UARTS).strip())
            record_property("harness_pio_registers", harness.run(HARNESS_PIOS).strip())
    assert not problems, f"{sample.target}: {problems}"
