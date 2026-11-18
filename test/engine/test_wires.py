# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness's PIO wire engines against native waveforms: a WS2812B's
# PULSE_BITS_IN capture judges each high pulse's width by the datasheet's
# windows (a 0 high 250-550 ns, a 1 high 650-950 ns, 1.25 us a bit; a low of
# 50 us ends the frame).

DIN = 6


def _frame(stimulus, at, data: bytes, zero_ns=400, one_ns=800):
    t = at
    for byte in data:
        for bit in range(7, -1, -1):
            stimulus.drive(DIN, 1, t)
            stimulus.drive(DIN, 0, t + (one_ns if byte >> bit & 1 else zero_ns))
            t += 1250
    return t


def _strip(harness):
    harness.run("import metalclaw\nfor i in metalclaw.instances():\n    i.detach()\n"
                f"strip = metalclaw.attach('ws2812b', 'strip', din=({DIN},))\nstrip.count = 1\n")


def test_a_ws2812b_frame_is_captured_from_its_pulses(harness, stimulus):
    _strip(harness)
    at = stimulus.until(stimulus.t_ns) + 100_000
    stimulus.drive(DIN, 0, at - 60_000)
    end = _frame(stimulus, at, bytes([0x10, 0x20, 0x30]))
    stimulus.until(end + 2_000_000)
    stimulus.go()
    assert harness.run("print(strip.frames, strip.bad_frames)").strip() == "1 0"
    assert harness.run("print(bytes(strip.read_region('leds', 0, 3)).hex())").strip() == "102030"


def test_a_pulse_outside_both_windows_spoils_the_frame(harness, stimulus):
    # 600 ns is neither a 0 (at most 550) nor a 1 (at least 650).
    _strip(harness)
    at = stimulus.until(stimulus.t_ns) + 100_000
    stimulus.drive(DIN, 0, at - 60_000)
    end = _frame(stimulus, at, bytes([0xFF, 0x00, 0xFF]), zero_ns=400, one_ns=600)
    stimulus.until(end + 2_000_000)
    stimulus.go()
    assert harness.run("print(strip.frames, strip.bad_frames)").strip() == "0 1"
