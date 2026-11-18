# SPDX-License-Identifier: Apache-2.0 OR MIT
# The host harness behind its bridge, with this test as the emulator: the
# models answer the DUT's I2C as they do on a bench, and their clock is the
# DUT's virtual time. Needs `just build-host`; skipped without it.

import pytest

from test.native.host import Bridge, HostHarness, default_build

PROGRAM = default_build()
pytestmark = pytest.mark.skipif(not PROGRAM.exists(), reason=f"no host harness at {PROGRAM} (just build-host)")

I2C0 = {"bus": 0, "sda": 4, "scl": 5}


@pytest.fixture
def bridged(tmp_path):
    path = tmp_path / "native.sock"
    harness = HostHarness(PROGRAM, path)
    bridge = Bridge(path)
    bridge.hello(i2c=[I2C0])
    yield harness, bridge
    bridge.close()
    harness.close()


def test_a_standalone_harness_lists_its_models():
    harness = HostHarness(PROGRAM)
    try:
        assert int(harness.run("import metalclaw\nprint(len(metalclaw.models()))")) > 50
    finally:
        harness.close()


def test_an_address_nothing_answers_is_not_acknowledged(bridged):
    _, bridge = bridged
    assert bridge.i2c(0, 0x18, write=b"\x06", read=2) is None


def test_the_mcp9808_answers_its_ids_on_the_dut_bus(bridged):
    # DS25095A registers 5-5 and 5-6: manufacturer 0054h, device 0400h, MSB
    # first, read after a pointer write.
    harness, bridge = bridged
    harness.run("import metalclaw\nt = metalclaw.attach('mcp9808', 't', i2c=(4, 5, 0x18))")
    assert bridge.i2c(0, 0x18, write=b"\x06", read=2) == b"\x00\x54"
    assert bridge.i2c(0, 0x18, write=b"\x07", read=2) == b"\x04\x00"
    # The pointer is kept: a read with no pointer write re-reads register 7.
    assert bridge.i2c(0, 0x18, read=2) == b"\x04\x00"


def test_a_dut_write_lands_in_the_model(bridged):
    # Register 2, T_UPPER, takes the word the DUT writes.
    harness, bridge = bridged
    harness.run("import metalclaw\nt = metalclaw.attach('mcp9808', 't', i2c=(4, 5, 0x18))")
    assert bridge.i2c(0, 0x18, write=b"\x02\x01\xe0") == b""
    assert bridge.i2c(0, 0x18, write=b"\x02", read=2) == b"\x01\xe0"


def test_the_runtime_clock_is_the_duts_virtual_time(bridged):
    harness, bridge = bridged
    harness.run("import metalclaw")
    before = int(harness.run("print(metalclaw.now_us())"))
    bridge.advance(250_000_000)
    after = int(harness.run("print(metalclaw.now_us())"))
    assert 249_000 <= after - before <= 251_000
    # Standing still in virtual time, it stands still however long we wait.
    import time
    time.sleep(0.2)
    assert int(harness.run("print(metalclaw.now_us())")) == after
