#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR MIT
# The rig: picks a platform, finds every case location, runs them with pytest.
#
#   test/rig/run.py --platform hardware [pytest args...]
#
# Cases live in test/cases/, devices/*/test/cases.py (or cases/), and
# client/test/cases.py. They never import a platform; they use the `harness`
# and `dut` fixtures the platform provides.

import argparse
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PLATFORMS = {
    "emulator": "test.rig.platforms.emulator.platform",
    "hardware": "test.rig.platforms.hardware.platform",
}


def case_locations() -> list[str]:
    found = []
    top = ROOT / "test" / "cases"
    if top.is_dir():
        found.append(str(top))
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
    argv = ["-p", PLATFORMS[args.platform], "--rootdir", str(ROOT), "--import-mode=importlib",
            "-o", "python_files=cases.py test_*.py"]
    argv += pytest_args
    argv += case_locations()
    return pytest.main(argv)


if __name__ == "__main__":
    sys.path.insert(0, str(ROOT))
    sys.exit(main())
