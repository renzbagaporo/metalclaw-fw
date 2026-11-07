# SPDX-License-Identifier: Apache-2.0 OR MIT

import inspect

from test.rig.platforms.emulator import platform


def test_the_emulated_harness_waits_for_serial_ports_as_long_as_for_enumeration():
    default = inspect.signature(platform.EmulatedHarness.serial_port).parameters["timeout"].default
    assert default == platform.ENUMERATION_TIMEOUT_S
