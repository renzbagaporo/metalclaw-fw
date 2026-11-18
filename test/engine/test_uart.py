# SPDX-License-Identifier: Apache-2.0 OR MIT
# The harness's UART engines against a native UART: a NEO-M8N's NMEA comes
# out of the harness bit by bit, and a UBX poll sent bit by bit is answered.
# On GP8/GP9 the harness uses its UART1 block; on GP10/GP11, which no UART
# block reaches as TX/RX, its PIO UART.

import pytest

from test.engine.stimulus import Uart

ENGINES = {"block": (8, 9), "pio": (10, 11)}
UBX_MON_VER = bytes([0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34])


@pytest.fixture(params=sorted(ENGINES), scope="module")
def gps(request, harness, stimulus):
    tx, rx = ENGINES[request.param]
    harness.run("import metalclaw\nfor i in metalclaw.instances():\n    i.detach()\n"
                f"gps = metalclaw.attach('neo_m8n', 'gps', uart=({tx}, {rx}))\n")
    # The harness's TX is this side's receive line, and the other way round.
    return Uart(stimulus, to_harness=rx, from_harness=tx)


def test_the_parts_nmea_comes_out_bit_by_bit(gps, stimulus):
    start = stimulus.until(stimulus.t_ns)
    stimulus.until(start + 1_500_000_000)
    stimulus.go()
    text = gps.received(since_ns=start).decode(errors="replace")
    assert "$G" in text and "\r\n" in text, text[:200]


def test_a_ubx_poll_sent_bit_by_bit_is_answered(gps, stimulus):
    start = stimulus.until(stimulus.t_ns)
    stimulus.go()
    end = gps.send(UBX_MON_VER)
    stimulus.until(end + 500_000_000)
    stimulus.go()
    assert b"\xb5\x62\x0a\x04" in gps.received(since_ns=start)
