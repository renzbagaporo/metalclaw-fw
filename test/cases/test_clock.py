# SPDX-License-Identifier: Apache-2.0 OR MIT


def clock(harness, code):
    return [int(v) for v in harness.run("import metalclaw, time\n" + code).split()]


def test_a_second_pause_leaves_the_clock_where_it_stopped(harness):
    try:
        first, second, now = clock(harness, """a = metalclaw.pause()
time.sleep_ms(50)
b = metalclaw.pause()
print(a, b, metalclaw.now_us())
""")
    finally:
        harness.run("import metalclaw\nmetalclaw.resume()\n")
    assert first == second == now


def test_a_second_resume_never_steps_the_clock_back(harness):
    readings = clock(harness, """metalclaw.resume()
out = []
for _ in range(20):
    time.sleep_us(300)
    out.append(metalclaw.now_us())
    metalclaw.resume()
    out.append(metalclaw.now_us())
print(*out)
""")
    assert readings == sorted(readings)
