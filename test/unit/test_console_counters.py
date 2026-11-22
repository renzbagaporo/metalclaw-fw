# SPDX-License-Identifier: Apache-2.0 OR MIT
# The counters that tell where a console's bytes were lost: the UART
# receiver's own FIFO, the port's receive ring, or the host queue's write.

import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BRIDGE = ROOT / "devices" / "console_bridge"
DRIVER = Path(__file__).with_name("console_counters_check.c")


def strict_options():
    return re.findall(r"^\s+(-W\S+)$", (ROOT / "core" / "cmake" / "strict.cmake").read_text(), re.M)


@pytest.fixture(scope="module")
def check(tmp_path_factory):
    exe = tmp_path_factory.mktemp("console_counters") / "console_counters_check"
    build = subprocess.run(["cc", "-O1", *strict_options(), "-I", ROOT / "core" / "include",
                            "-I", ROOT / "core" / "ports" / "rp2" / "hal", "-I", BRIDGE,
                            DRIVER, BRIDGE / "console_bridge.c", "-o", exe], capture_output=True, text=True)
    assert build.returncode == 0, build.stderr

    def run(*args):
        done = subprocess.run([exe, *args], capture_output=True, text=True, timeout=30)
        assert done.returncode == 0, (done.returncode, done.stderr)
        return {k: int(v) for k, v in (field.split("=") for field in done.stdout.split())}

    return run


def test_a_clean_byte_is_received_and_counts_nothing_lost(check):
    assert check("uart", "...") == {"ringed": 3, "bytes_rx": 3, "overruns": 0, "fifo_overruns": 0,
                                    "ring_overruns": 0, "frame_errors": 0}


def test_a_receiver_overflow_keeps_the_byte_it_came_with_and_counts_as_fifo(check):
    assert check("uart", ".o.") == {"ringed": 3, "bytes_rx": 3, "overruns": 1, "fifo_overruns": 1,
                                    "ring_overruns": 0, "frame_errors": 0}


def test_a_full_ring_drops_the_byte_and_counts_as_ring(check):
    assert check("uart", ".rr") == {"ringed": 1, "bytes_rx": 1, "overruns": 2, "fifo_overruns": 0,
                                    "ring_overruns": 2, "frame_errors": 0}


def test_overruns_is_the_sum_of_both_and_a_bad_frame_is_neither(check):
    assert check("uart", "bf.") == {"ringed": 1, "bytes_rx": 1, "overruns": 2, "fifo_overruns": 1,
                                    "ring_overruns": 1, "frame_errors": 1}


def test_each_hand_off_the_host_queue_takes_short_is_counted_once(check):
    got = check("bridge", "all", "3", "0", "all")
    assert {k: got[k] for k in ("to_host", "overruns", "short_writes", "reset_short_writes")} == {
        "to_host": 19, "overruns": 13, "short_writes": 2, "reset_short_writes": 0}


def test_a_dut_break_reaches_the_host_and_a_host_break_the_dut(check):
    # 115200 baud: a 2 ms SEND_BREAK is 230 bit times; a 0 ms one asks for
    # nothing; one held until further notice (FFFFh) is a LIN break's 13.
    got = check("bridge", "all", "B", "S2", "S65535")
    assert (got["breaks_to_host"], got["usb_breaks"], got["breaks_from_host"], got["break_bits"]) == (1, 1, 2, 13)
