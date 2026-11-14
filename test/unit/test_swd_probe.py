# SPDX-License-Identifier: Apache-2.0 OR MIT

import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
PROBE = ROOT / "devices" / "swd_probe"
DRIVER = Path(__file__).with_name("swd_probe_usb_rx.c")
# DAP_Info(vendor): the command, the string's length with its NUL, the string.
VENDOR_REPLY = bytes([0x00, 0x0A]) + b"Metalclaw\0"


def strict_options():
    return re.findall(r"^\s+(-W\S+)$", (ROOT / "core" / "cmake" / "strict.cmake").read_text(), re.M)


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    exe = tmp_path_factory.mktemp("swd_probe") / "swd_probe_usb_rx"
    sources = [DRIVER, PROBE / "swd_probe.c", PROBE / "dap.c"]
    build = subprocess.run(["cc", "-O1", *strict_options(), "-I", ROOT / "core" / "include", "-I", PROBE,
                            *sources, "-o", exe], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr

    def run(*takes):
        done = subprocess.run([exe, *takes], capture_output=True, text=True, timeout=30)
        assert done.returncode == 0, (done.returncode, done.stderr)
        return [dict(field.split("=", 1) for field in line.split() if "=" in field)
                for line in done.stdout.splitlines()]

    return run


def test_a_reply_the_interface_takes_whole_is_not_counted(probe):
    request, after_reset = probe("all")
    assert request == {"offered": "12", "taken": "12", "short_replies": "0", "reply": VENDOR_REPLY.hex()}
    assert after_reset["short_replies"] == "0"


def test_a_reply_cut_short_is_counted_and_keeps_its_head(probe):
    request, _ = probe("5")
    assert request == {"offered": "12", "taken": "5", "short_replies": "1", "reply": VENDOR_REPLY[:5].hex()}


def test_each_reply_refused_outright_is_counted(probe):
    first, second, _ = probe("0", "0")
    assert (first["taken"], first["short_replies"]) == ("0", "1")
    assert (second["taken"], second["short_replies"]) == ("0", "2")


def test_a_whole_reply_after_a_short_one_leaves_the_count(probe):
    short, whole, _ = probe("3", "all")
    assert short["short_replies"] == "1"
    assert (whole["taken"], whole["short_replies"]) == ("12", "1")


def test_reset_clears_the_count(probe):
    *_, after_reset = probe("0", "11")
    assert after_reset == {"short_replies": "0"}
