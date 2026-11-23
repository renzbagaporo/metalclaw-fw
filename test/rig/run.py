#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR MIT
# The rig: picks a platform, finds every case location, runs them with pytest.
#
#   test/rig/run.py --platform hardware [pytest args...]
#
# Cases live in test/cases/, devices/*/test/cases.py (or cases/),
# client/test/cases.py, and test/zephyr_samples/cases.py. They never import
# a platform; they use the `harness`, `dut` and `sample` fixtures the
# platform provides.

import argparse
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PLATFORMS = {
    "hardware": "test.rig.platforms.hardware.platform",
    "sim": "test.rig.platforms.sim.platform",
}


def case_locations() -> list[str]:
    found = [str(ROOT / "test" / "cases"), str(ROOT / "test" / "zephyr_samples" / "cases.py")]
    for holder in ("devices", "client"):
        for cases in sorted((ROOT / holder).glob("*/test/cases.py")) + sorted((ROOT / holder).glob("*/test/cases")):
            found.append(str(cases))
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", required=True, choices=sorted(PLATFORMS))
    args, pytest_args = parser.parse_known_args()

    import pytest

    os.environ["METALCLAW_PLATFORM"] = args.platform
    # probe_telemetry first: the platform imports it, and pytest warns about
    # a plugin module it finds already imported.
    argv = ["-p", "test.rig.probe_telemetry", "-p", PLATFORMS[args.platform], "-p", "test.rig.host_load",
            "--rootdir", str(ROOT), "--import-mode=importlib",
            "-o", "python_files=cases.py test_*.py"]
    argv += pytest_args
    argv += case_locations()
    return pytest.main(argv)


if __name__ == "__main__":
    sys.path.insert(0, str(ROOT))
    sys.exit(main())
