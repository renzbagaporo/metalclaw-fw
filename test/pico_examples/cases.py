# SPDX-License-Identifier: Apache-2.0 OR MIT
# An unmodified pico-examples program on the DUT, judged by its stdio UART
# against test/pico_examples/targets.py. The harness bridges the DUT's
# console UART (fixture "uart") to a serial port on this host, the program
# restarts from reset with the port open, and the case reads until every
# expected line is in or the program's time is up on the harness's clock.

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


def read_console(harness, port, spec) -> str:
    # The clock is a REPL round trip, so it is read only while the port idles.
    got = bytearray()
    deadline = harness.clock() + spec.seconds
    while True:
        chunk = port.read(4096)
        got += chunk
        if chunk:
            if not spec.match(got.decode(errors="replace"))[0]:
                break
        elif harness.clock() >= deadline:
            break
    return got.decode(errors="replace")


def test_the_example_completes(harness, example, console):
    spec = SPECS.get(example.target) or PART_SPECS.get(example.target)
    if spec is None:
        pytest.skip(f"no completion criteria for {example.target} yet")
    with serial.Serial(console, BAUD, timeout=0.1, exclusive=True) as port:
        port.reset_input_buffer()
        example.restart()
        text = read_console(harness, port, spec)
    missing, forbidden = spec.match(text)
    assert not forbidden, f"{example.target} printed {forbidden}:\n{text[-2000:]}"
    assert not missing, f"{example.target} never printed {missing} within {spec.seconds} s:\n{text[-2000:]}"


def test_the_example_drives_its_pins(example, parts):
    spec = PIN_SPECS.get(example.target)
    if spec is None:
        pytest.skip(f"no pin criteria for {example.target}")
    example.restart()
    samples = pins.watch(example.memory, example.metadata["chip"], spec.seconds)
    problems = [p for toggle in spec.toggles for p in toggle.check(samples)]
    assert not problems, f"{example.target}: {problems} ({len(samples)} samples over {spec.seconds} s)"
