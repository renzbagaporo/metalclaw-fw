# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness in Renode as the cases use a harness: the hardware platform's
# Harness over the imported USB, with its waits stretched for emulated time.

import math
import os

from test.rig.platforms.hardware.platform import Harness

# Room for the cable to replug a few times (bench.REPLUG_AFTER_S).
ENUMERATION_TIMEOUT_S = 600.0

# Cases give REPL deadlines in bench seconds. The emulated harness does the
# same work in the same emulated time, which passes about 80 times slower
# than host time.
TIME_SCALE_ENV = "METALCLAW_RENODE_TIME_SCALE"
TIME_SCALE = 150.0


def time_scale() -> float:
    value = os.environ.get(TIME_SCALE_ENV, TIME_SCALE)
    try:
        scale = float(value)
    except (TypeError, ValueError, OverflowError):
        scale = math.nan
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError(f"invalid {TIME_SCALE_ENV} {value!r}; expected a positive finite factor")
    return scale


# Attaching a device that adds a USB function re-enumerates the harness, so
# its serial ports reappear on the same schedule as the harness itself.
class EmulatedHarness(Harness):
    def wait_enumerated(self, timeout: float = ENUMERATION_TIMEOUT_S) -> None:
        super().wait_enumerated(timeout)

    def serial_port(self, interface: str, timeout: float = ENUMERATION_TIMEOUT_S) -> str:
        return super().serial_port(interface, timeout)

    def wait_interfaces(self, settled, what: str, timeout: float = ENUMERATION_TIMEOUT_S) -> None:
        super().wait_interfaces(settled, what, timeout)

    def wait_interface(self, name: str, present: bool = True, timeout: float = ENUMERATION_TIMEOUT_S) -> None:
        super().wait_interface(name, present, timeout)

    def run(self, code: str, timeout: float = 5.0) -> str:
        return super().run(code, timeout * time_scale())
