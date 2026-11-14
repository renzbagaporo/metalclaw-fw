# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness USB layer's vendor backlog (core/ports/rp2/usb/vendor_backlog.h),
# built for the host with the firmware's warnings.

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
USB = ROOT / "core" / "ports" / "rp2" / "usb"
CHECK = Path(__file__).with_name("vendor_backlog_check.c")


def strict_options():
    return re.findall(r"^\s+(-W\S+)$", (ROOT / "core" / "cmake" / "strict.cmake").read_text(), re.M)


def test_the_backlog_keeps_order_through_wrap_and_overflow(tmp_path):
    exe = tmp_path / "vendor_backlog_check"
    build = subprocess.run(["cc", "-O1", *strict_options(), "-I", USB, CHECK, "-o", exe],
                           capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([exe], capture_output=True, text=True, timeout=30)
    assert (run.returncode, run.stdout.strip()) == (0, "ok"), run.stdout
