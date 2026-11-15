# SPDX-License-Identifier: Apache-2.0 OR MIT
# An unmodified pico-examples program on the DUT, judged by its stdio UART
# against test/pico_examples/targets.py. The harness bridges the DUT's
# console UART (fixture "uart") to a serial port on this host, the program
# restarts from reset with the port open, and the case reads until every
# expected line is in or the program's time is up on the harness's clock.

import pytest
import serial

from test.pico_examples.targets import SPECS

NAME = "example_console"
INTERFACE = "Metalclaw DUT console"
BAUD = 115200

DETACH = f"""import metalclaw
for instance in metalclaw.instances():
    if instance.name == {NAME!r}:
        instance.detach()
"""


@pytest.fixture(scope="module")
def console(harness, example, fixture):
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
    spec = SPECS.get(example.target)
    if spec is None:
        pytest.skip(f"no completion criteria for {example.target} yet")
    with serial.Serial(console, BAUD, timeout=0.1, exclusive=True) as port:
        port.reset_input_buffer()
        example.reset()
        text = read_console(harness, port, spec)
    missing, forbidden = spec.match(text)
    assert not forbidden, f"{example.target} printed {forbidden}:\n{text[-2000:]}"
    assert not missing, f"{example.target} never printed {missing} within {spec.seconds} s:\n{text[-2000:]}"
