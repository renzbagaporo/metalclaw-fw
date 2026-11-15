# SPDX-License-Identifier: Apache-2.0 OR MIT
# An unmodified pico-examples program on the DUT, judged by its stdio UART
# against test/pico_examples/targets.py. The harness bridges the DUT's
# console UART (fixture "uart") to a serial port on this host, the program
# restarts from reset with the port open, and the case reads until every
# expected line is in or the program's time is up on the harness's clock.

import ast
import contextlib
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
# restarts it, and gone once its cases are done; a part the bench already
# provides (example.replaced_parts) is left out. A part on a DUT pin no
# harness pin reaches on this bench (fixture 1's GP2, GP3 and GP28), or one
# that needs a pull-up this bench's lines lack, leaves the program without
# its outside world, so none of its cases can judge it there.
@pytest.fixture(scope="module")
def parts(harness, example, fixture):
    attached = parts_for(example.target, example.metadata.get("board"), example.replaced_parts)
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
# due.
def read_console(harness, port, spec, patterns=None, inputs=None, ports=None) -> str:
    got = bytearray()
    deadline = harness.clock() + max(spec.seconds, MIN_SECONDS)
    checked = time.monotonic()
    typist = Typist(port, spec.input if inputs is None else inputs, harness, ports)
    while True:
        chunk = port.read(4096)
        got += chunk
        if chunk:
            typist(got.decode(errors="replace"))
        if chunk and not spec.match(got.decode(errors="replace"), patterns)[0]:
            break
        if not chunk or time.monotonic() - checked >= CLOCK_EVERY_S:
            checked = time.monotonic()
            if harness.clock() >= deadline:
                break
    return got.decode(errors="replace")


def test_the_example_completes(harness, example, fixture, console):
    spec = SPECS.get(example.target) or PART_SPECS.get(example.target)
    if spec is None:
        pytest.skip(f"no completion criteria for {example.target} yet")
    board = example.metadata.get("board")
    spec = spec.for_board(board)
    if spec is None:
        pytest.skip(f"{example.target}'s console cannot judge its {board} build")
    spec = spec.for_bench(fixture["line_pullups"], fixture.get("analog_pins", ()))
    if not spec.console and spec.usb_console:
        pytest.skip(f"{example.target} prints only on its own USB console")
    with serial.Serial(console, BAUD, timeout=0.1, exclusive=True) as port:
        port.reset_input_buffer()
        example.restart()
        text = read_console(harness, port, spec)
    missing, forbidden = spec.match(text)
    assert not forbidden, f"{example.target} printed {forbidden}:\n{text[-2000:]}"
    assert not missing, (f"{example.target} never printed {missing} within {max(spec.seconds, MIN_SECONDS)} s:"
                         f"\n{text[-2000:]}")


# What a PinSpec does to the DUT while its pins are watched: bytes on the
# console, presses of the board's BOOTSEL button.
def pin_actions(spec, chip, port):
    actions = [(at, lambda link, data=data: (port.write(data), port.flush())) for at, data in spec.send]
    for press in spec.presses:
        actions.append((press.at, lambda link: pins.bootsel(link, chip, True)))
        actions.append((press.release, lambda link: pins.bootsel(link, chip, False)))
    return sorted(actions, key=lambda a: a[0])


def test_the_example_drives_its_pins(example, parts, console):
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
    assert not problems, f"{example.target}: {problems} ({len(samples)} samples over {spec.seconds} s)"


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


def test_the_example_parts_saw_the_program(harness, example, parts):
    checks = [(part, check) for part in parts for check in part.checks]
    if not checks:
        # A stand-in's pass is never a harness part's: what the part would
        # have seen is not judged at all.
        stood_in = [p.name for p in parts_for(example.target, example.metadata.get("board"))
                    if p.name in example.replaced_parts and p.checks]
        if stood_in:
            pytest.skip(f"{example.target}'s part checks read the harness's {', '.join(stood_in)}, which an "
                        f"emulator-only stand-in replaces in this session")
        pytest.skip(f"no part checks for {example.target}")
    example.restart()
    for part, check in checks:
        if check.setup:
            harness.run(f"part = {part.name}\n{check.setup}\n")
    harness.sleep(max(check.after_seconds for _, check in checks))
    problems = []
    for part, check in checks:
        value = ast.literal_eval(harness.run(f"part = {part.name}\nprint(repr({check.expression}))\n").strip())
        if not check.test(value):
            problems.append(f"{part.name}: {check.description} (got {value!r:.200})")
    assert not problems, f"{example.target}: {problems}"


# The registers that show the program set up what its pins do, read over
# SWD a moment after it starts (pin_specs.REGISTER_SPECS).
def test_the_example_sets_its_registers(harness, example, parts):
    spec = REGISTER_SPECS.get(example.target)
    if spec is None:
        pytest.skip(f"no register criteria for {example.target}")
    chip = example.metadata["chip"]
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
    spec = spec.for_board(board)
    if spec is None:
        pytest.skip(f"{example.target}'s console cannot judge its {board} build")
    spec = spec.for_bench(fixture["line_pullups"], fixture.get("analog_pins", ()))
    assert dut_usb.ttys, f"{example.target} presents no CDC ACM function on {dut_usb.device}"
    inputs = tuple((pattern, (name, data)) for pattern, name, data in spec.usb_input)
    # Opening a CDC function raises its DTR, which stdio_usb waits for: a
    # program prints nothing there before.
    with contextlib.ExitStack() as stack:
        ports = {name: stack.enter_context(serial.Serial(node, BAUD, timeout=0.1, exclusive=True))
                 for name, node in dut_usb.ttys}
        text = read_console(harness, ports[dut_usb.ttys[0][0]], spec, spec.usb_console, inputs, ports)
    missing, forbidden = spec.match(text, spec.usb_console)
    assert not forbidden, f"{example.target} printed {forbidden} on its USB console:\n{text[-2000:]}"
    assert not missing, (f"{example.target} never printed {missing} on its USB console within "
                         f"{max(spec.seconds, MIN_SECONDS)} s:\n{text[-2000:]}")
