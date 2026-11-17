# SPDX-License-Identifier: Apache-2.0 OR MIT
# An unmodified pico-examples program on the DUT, judged by its stdio UART
# against test/pico_examples/targets.py. The harness bridges the DUT's
# console UART (fixture "uart") to a serial port on this host, the program
# restarts from reset with the port open, and the case reads until every
# expected line is in or the program's time is up on the harness's clock.

import ast
import contextlib
import os
import re
import time

import pytest
import serial

from test.pico_examples import pins
from test.pico_examples.part_targets import PART_SPECS
from test.pico_examples.parts import PinError, parts_for
from test.pico_examples.pin_specs import BOOTSEL_SPECS, PIN_SPECS, REGISTER_SPECS
from test.pico_examples.targets import SPECS
from test.pico_examples.usb_devices import usb_device

NAME = "example_console"
INTERFACE = "Metalclaw DUT console"
BAUD = 115200

DETACH = f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
"""


# The program's external parts, attached on the harness before any case
# restarts it, and gone once its cases are done. A part on a DUT pin no
# harness pin reaches on this bench (fixture 1's GP2, GP3 and GP28), or one
# that needs a pull-up this bench's lines lack, leaves the program without
# its outside world, so none of its cases can judge it there.
@pytest.fixture(scope="module")
def parts(harness, example, fixture):
    attached = parts_for(example.target, example.metadata.get("board"))
    for part in attached:
        if part.needs_line_pullups and not fixture["line_pullups"]:
            pytest.skip(f"{example.target}'s {part.model} needs a pull-up on its line, and this bench has none")
    try:
        codes = [part.code(fixture) for part in attached]
    except PinError as error:
        pytest.skip(f"{example.target} needs a pin this bench does not reach: {error}")
    for code in codes:
        harness.run(code)
    yield attached
    for part in attached:
        harness.run(f"import metalclaw\nfor i in metalclaw.instances():\n    if i.name == {part.name!r}: i.detach()\n")


# Only for a program whose console or pins are judged: one that prints
# nothing and has no pin criteria may drive the console pins for something
# else (pio_hub75's R0 and G0).
@pytest.fixture(scope="module")
def console(harness, example, fixture, parts):
    if not any(example.target in specs for specs in (SPECS, PART_SPECS, PIN_SPECS)):
        pytest.skip(f"no completion criteria for {example.target} yet")
    uart = fixture["uart"]
    harness.run(DETACH + f"metalclaw.attach('console_bridge', {NAME!r}, uart={(uart['tx'], uart['rx'])})\n")
    harness.wait_interface(INTERFACE)
    yield harness.serial_port(INTERFACE)
    harness.run(DETACH)
    harness.wait_interface(INTERFACE, present=False)


# The clock is a REPL round trip, so it is read while the port idles, and
# otherwise at most every CLOCK_EVERY_S of host time: a program that prints
# without pause still ends on its deadline.
CLOCK_EVERY_S = 0.5

# A file the console is appended to as it arrives, so that a session killed
# before its report still leaves what the program printed.
CONSOLE_LOG_ENV = "METALCLAW_CONSOLE_LOG"


def console_log():
    path = os.environ.get(CONSOLE_LOG_ENV)
    return open(path, "ab", buffering=0) if path else contextlib.nullcontext()


# Every console read gets at least this many bench seconds: a read ends as
# soon as everything matched, so a long window costs time only on a failure,
# while a short one fails a program whose loop runs slower than its source's
# own delays suggest.
MIN_SECONDS = 10.0


class Typist:
    # Sends each of a Spec's inputs once its pattern has come in, after where
    # the one before it matched: bytes to the console, a str to the harness
    # as code, for a part that is to act then, (name, bytes) to the port of
    # that name in `ports`.
    def __init__(self, port, inputs, harness=None, ports=None):
        self.port = port
        self.harness = harness
        self.ports = ports or {}
        self.inputs = list(inputs)
        self.at = 0

    def __call__(self, text: str) -> None:
        while self.inputs:
            pattern, data = self.inputs[0]
            m = re.compile(pattern).search(text, self.at)
            if m is None:
                return
            if isinstance(data, str):
                self.harness.run(data)
            else:
                port = self.port
                if isinstance(data, tuple):
                    name, data = data
                    port = self.ports[name]
                port.write(data)
                port.flush()
            self.at = m.end()
            self.inputs.pop(0)


# Until `patterns` (the Spec's console unless given) all matched, or the
# Spec's time is up; `inputs` (its own unless given) are typed as they come
# due. With `arrivals`, when each chunk came in on the bench's clock, for
# Spec.timing: (characters read by then, a reading taken before a read that
# found nothing ahead of the chunk, the first reading after it). A chunk
# that a read returns came in after any earlier read that found nothing
# had returned, and before the next clock reading; the bridge's own delay
# is taken as the same for every line.
def read_console(harness, port, spec, patterns=None, inputs=None, ports=None, arrivals=None) -> str:
    typist = Typist(port, spec.input if inputs is None else inputs, harness, ports)
    with console_log() as log:
        return _read_console(harness, port, spec, patterns, typist, arrivals, log)


def _read_console(harness, port, spec, patterns, typist, arrivals, log) -> str:
    got = bytearray()
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


def test_the_example_completes(harness, example, fixture, console, record_property):
    spec = SPECS.get(example.target) or PART_SPECS.get(example.target)
    if spec is None:
        pytest.skip(f"no completion criteria for {example.target} yet")
    board = example.metadata.get("board")
    judged = spec.for_board(board)
    if judged is None:
        pytest.skip(spec.why_not(example.target, board))
    spec = judged.for_bench(fixture["line_pullups"], fixture.get("analog_pins", ()))
    if not spec.console and spec.usb_console:
        pytest.skip(f"{example.target} prints only on its own USB console")
    # An empty read shows nothing: a program that never ran prints nothing
    # too.
    if not spec.console:
        pytest.skip(f"{example.target} prints nothing on its console; its parts or pins judge it")
    arrivals = []
    with serial.Serial(console, BAUD, timeout=0.1, exclusive=True) as port:
        port.reset_input_buffer()
        example.restart()
        text = read_console(harness, port, spec, arrivals=arrivals)
    missing, forbidden = spec.match(text)
    if missing or forbidden:
        # The whole console, for the junit report: the assertion keeps only
        # its end.
        record_property("console", text)
    assert not forbidden, f"{example.target} printed {forbidden}:\n{text[-2000:]}"
    assert not missing, (f"{example.target} never printed {missing} within {max(spec.seconds, MIN_SECONDS)} s:"
                         f"\n{text[-2000:]}")
    late = spec.timing(text, arrivals)
    assert not late, f"{example.target}: {late}:\n{text[-2000:]}"


# What a PinSpec does to the DUT while its pins are watched: bytes on the
# console, presses of the board's BOOTSEL button.
def pin_actions(spec, chip, port):
    actions = [(at, lambda link, data=data: (port.write(data), port.flush())) for at, data in spec.send]
    for press in spec.presses:
        actions.append((press.at, lambda link: pins.bootsel(link, chip, True)))
        actions.append((press.release, lambda link: pins.bootsel(link, chip, False)))
    return sorted(actions, key=lambda a: a[0])


def test_the_example_drives_its_pins(example, parts, console, record_property):
    spec = PIN_SPECS.get(example.target)
    if spec is None:
        pytest.skip(f"no pin criteria for {example.target}")
    chip = example.metadata["chip"]
    watched = sorted({check.pin for check in spec.toggles})
    with serial.Serial(console, BAUD, timeout=0.1, exclusive=True) as port:
        example.restart()
        try:
            samples = pins.watch(example.memory, chip, spec.seconds, watched,
                                 actions=pin_actions(spec, chip, port))
        finally:
            # A press left held reads as BOOTSEL to a boot that leaves
            # IO_QSPI as it was.
            if spec.presses:
                with example.memory.link() as link:
                    pins.bootsel(link, chip, False)
    problems = [p for check in spec.toggles for p in check.check(samples)]
    if problems:
        # Each watched pin's changes, in seconds into the watch, for the
        # junit report: the message names only the intervals that failed.
        for pin in watched:
            record_property(f"pin_GP{pin}_changes",
                            " ".join(f"{t:.6f}:{level}" for t, level in pins.edges(samples, pin)[:PIN_CHANGES_KEPT]))
    assert not problems, f"{example.target}: {problems} ({len(samples)} samples over {spec.seconds} s)"


# How many of each pin's changes a failed pin check records.
PIN_CHANGES_KEPT = 64


# A program linked with pico_bootsel_via_double_reset: one reset on RUN
# starts it as usual, two within its window enter the USB bootloader. The
# probe drives RUN (the fixture's swd nreset) with its own timing.
def test_the_example_enters_bootsel_on_a_double_reset(example, parts):
    spec = BOOTSEL_SPECS.get(example.target)
    if spec is None:
        pytest.skip(f"no double-reset criteria for {example.target}")
    chip = example.metadata["chip"]
    try:
        for resets, expect_bootsel in ((1, False), (2, True)):
            if resets == 1:
                # The program blinks once its double-reset window is over.
                settled = pins.watch(example.memory, chip, spec.seconds, (spec.led,))
                assert pins.edges(settled, spec.led), (
                    f"{example.target}: GP{spec.led} never changed before the single reset")
            example.reset_by_run(resets, spec.low_s, spec.gap_s)
            samples = pins.watch(example.memory, chip, spec.seconds, (spec.led,))
            with example.memory.link() as link:
                presenting = pins.usb_presenting(link, chip)
            seen = len(pins.edges(samples, spec.led))
            assert presenting == expect_bootsel, (
                f"{example.target}: after {resets} RUN reset(s) the DUT's USB is "
                f"{'' if presenting else 'not '}presenting, GP{spec.led} changed {seen} times")
            if expect_bootsel:
                assert seen <= 1, f"{example.target}: GP{spec.led} changed {seen} times in the bootloader"
            else:
                assert seen >= spec.edges, f"{example.target}: GP{spec.led} changed {seen} times after one reset"
    finally:
        example.restart()


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


def test_the_example_parts_saw_the_program(harness, example, parts, record_property):
    checks = [(part, check) for part in parts for check in part.checks]
    if not checks:
        pytest.skip(f"no part checks for {example.target}")
    example.restart()
    for part, check in checks:
        if check.setup:
            harness.run(f"part = {part.name}\n{check.setup}\n")
    harness.sleep(max(check.after_seconds for _, check in checks))
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
        if "uart" in part.bindings:
            record_property("harness_uart_registers", harness.run(HARNESS_UARTS).strip())
            record_property("harness_pio_registers", harness.run(HARNESS_PIOS).strip())
    assert not problems, f"{example.target}: {problems}"


# The registers that show the program set up what its pins do, or the OTP
# rows it wrote, read over SWD a moment after it starts
# (pin_specs.REGISTER_SPECS).
def test_the_example_sets_its_registers(harness, example, parts):
    spec = REGISTER_SPECS.get(example.target)
    chip = example.metadata["chip"]
    if spec is None or chip not in spec.registers:
        pytest.skip(f"no register criteria for {example.target} on {chip}")
    example.restart()
    harness.sleep(spec.after)
    with example.memory.link() as link:
        values = [(register, link.read_words(register.address, 1)[0]) for register in spec.registers[chip]]
    problems = [p for register, value in values if (p := register.check(value)) is not None]
    assert not problems, f"{example.target}: {problems}"


# The program's device on the DUT's own USB, which the rig host reaches only
# on a bench that cables it (the `dut_usb` fixture): what its descriptors
# say it presents (usb_devices.py).
def test_the_example_presents_its_usb_device(example, dut_usb):
    device = usb_device(example.target, example.metadata["chip"])
    found = {"VID:PID": f"{dut_usb.vid:04x}:{dut_usb.pid:04x}", "ttys": tuple(name for name, _ in dut_usb.ttys),
             "hidraw": len(dut_usb.hidraw)}
    expected = {"VID:PID": f"{device.vid:04x}:{device.pid:04x}", "ttys": device.ttys, "hidraw": device.hidraw}
    if device.endpoints:
        found["endpoints"] = tuple(int(ep, 16) for ep in dut_usb.endpoints)
        expected["endpoints"] = tuple(sorted(device.endpoints))
    assert found == expected, f"{example.target} on {dut_usb.device}"


def test_the_example_completes_on_its_usb_console(harness, example, fixture, dut_usb):
    spec = SPECS.get(example.target) or PART_SPECS.get(example.target)
    if spec is None or not spec.usb_console:
        pytest.skip(f"no USB console criteria for {example.target}")
    board = example.metadata.get("board")
    judged = spec.for_board(board)
    if judged is None:
        pytest.skip(spec.why_not(example.target, board))
    spec = judged.for_bench(fixture["line_pullups"], fixture.get("analog_pins", ()))
    assert dut_usb.ttys, f"{example.target} presents no CDC ACM function on {dut_usb.device}"
    inputs = tuple((pattern, (name, data)) for pattern, name, data in spec.usb_input)
    # Opening a CDC function raises its DTR, which stdio_usb waits for: a
    # program prints nothing there before.
    with contextlib.ExitStack() as stack:
        ports = {name: stack.enter_context(serial.Serial(node, BAUD, timeout=0.1, exclusive=True))
                 for name, node in dut_usb.ttys}
        text = read_console(harness, ports[dut_usb.ttys[0][0]], spec, spec.usb_console, inputs, ports)
        replies = {name: read_reply(harness, ports[name], pattern, spec) for name, pattern in spec.usb_replies}
    missing, forbidden = spec.match(text, spec.usb_console)
    assert not forbidden, f"{example.target} printed {forbidden} on its USB console:\n{text[-2000:]}"
    assert not missing, (f"{example.target} never printed {missing} on its USB console within "
                         f"{max(spec.seconds, MIN_SECONDS)} s:\n{text[-2000:]}")
    for name, pattern in spec.usb_replies:
        assert re.search(pattern, replies[name]), (f"{example.target} never answered {pattern!r} on {name!r} within "
                                                   f"{max(spec.seconds, MIN_SECONDS)} s: {replies[name][-500:]!r}")


# What one CDC function answers on its own tty, until `pattern` matched or
# the Spec's seconds passed on the bench's clock.
def read_reply(harness, port, pattern: str, spec) -> str:
    got = bytearray()
    deadline = harness.clock() + max(spec.seconds, MIN_SECONDS)
    checked = time.monotonic()
    while not re.search(pattern, got.decode(errors="replace")):
        chunk = port.read(4096)
        got += chunk
        if not chunk or time.monotonic() - checked >= CLOCK_EVERY_S:
            checked = time.monotonic()
            if harness.clock() >= deadline:
                break
    return got.decode(errors="replace")
