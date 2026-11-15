# SPDX-License-Identifier: Apache-2.0 OR MIT
# An unmodified pico-examples program on the DUT, judged by its stdio UART
# against test/pico_examples/targets.py. The harness bridges the DUT's
# console UART (fixture "uart") to a serial port on this host, the program
# restarts from reset with the port open, and the case reads until every
# expected line is in or the program's time is up on the harness's clock.

import ast
import time

import pytest
import serial

from test.pico_examples import pins
from test.pico_examples.part_targets import PART_SPECS
from test.pico_examples.parts import PARTS
from test.pico_examples.pin_specs import PIN_SPECS
from test.pico_examples.targets import SPECS

NAME = "example_console"
INTERFACE = "Metalclaw DUT console"
BAUD = 115200

DETACH = f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
"""


# The program's external parts, attached on the harness before any case
# restarts it, and gone once its cases are done.
@pytest.fixture(scope="module")
def parts(harness, example, fixture):
    attached = PARTS.get(example.target, ())
    for part in attached:
        harness.run(part.code(fixture))
    yield attached
    for part in attached:
        harness.run(f"import metalclaw\nfor i in metalclaw.instances():\n    if i.name == {part.name!r}: i.detach()\n")


@pytest.fixture(scope="module")
def console(harness, example, fixture, parts):
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


def read_console(harness, port, spec) -> str:
    got = bytearray()
    deadline = harness.clock() + max(spec.seconds, MIN_SECONDS)
    checked = time.monotonic()
    while True:
        chunk = port.read(4096)
        got += chunk
        if chunk and not spec.match(got.decode(errors="replace"))[0]:
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
    spec = spec.for_bench(fixture["line_pullups"])
    with serial.Serial(console, BAUD, timeout=0.1, exclusive=True) as port:
        port.reset_input_buffer()
        example.restart()
        text = read_console(harness, port, spec)
    missing, forbidden = spec.match(text)
    assert not forbidden, f"{example.target} printed {forbidden}:\n{text[-2000:]}"
    assert not missing, (f"{example.target} never printed {missing} within {max(spec.seconds, MIN_SECONDS)} s:"
                         f"\n{text[-2000:]}")


def test_the_example_drives_its_pins(example, parts):
    spec = PIN_SPECS.get(example.target)
    if spec is None:
        pytest.skip(f"no pin criteria for {example.target}")
    example.restart()
    watched = sorted({toggle.pin for toggle in spec.toggles})
    samples = pins.watch(example.memory, example.metadata["chip"], spec.seconds, watched)
    problems = [p for toggle in spec.toggles for p in toggle.check(samples)]
    assert not problems, f"{example.target}: {problems} ({len(samples)} samples over {spec.seconds} s)"


def test_the_example_parts_saw_the_program(harness, example, parts):
    checks = [(part, check) for part in parts for check in part.checks]
    if not checks:
        pytest.skip(f"no part checks for {example.target}")
    example.restart()
    harness.sleep(max(check.after_seconds for _, check in checks))
    problems = []
    for part, check in checks:
        value = ast.literal_eval(harness.run(f"part = {part.name}\nprint(repr({check.expression}))\n").strip())
        if not check.test(value):
            problems.append(f"{part.name}: {check.description} (got {value!r:.200})")
    assert not problems, f"{example.target}: {problems}"
