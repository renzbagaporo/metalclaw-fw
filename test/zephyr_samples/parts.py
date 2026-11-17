# SPDX-License-Identifier: Apache-2.0 OR MIT
# The Metalclaw parts an upstream Zephyr sample talks to, attached before it
# starts, as for a pico-examples program (test/pico_examples/parts.py): each
# binding names DUT pins, here the pins of the devicetree nodes the sample
# is built with (test/zephyr_samples/builds.py). Property values are chosen
# unlike the models' defaults, so what the sample prints shows it came from
# the part.

from test.pico_examples.parts import Attach, Check


# The sx1262 node of devices/sx126x/test/dut/rp2.overlay: SPI0 SCK GP18, MOSI
# GP19, MISO GP16, CS GP21 (a GPIO), NRESET GP6, BUSY GP10, DIO1 GP20. The
# radio's engine drives BUSY, so every pin comes from one PIO window (on the
# carrier, BUSY takes the GPIO22 branch). What it receives is set to the
# lora samples' channel (samples/drivers/lora/*/src/main.c: 865.1 MHz,
# SF10, 125 kHz, CR 4/5, preamble 8, private network), with a signal of
# -73 dBm at 7 dB SNR.
def _sx1262(**checks) -> Attach:
    return Attach("sx1262", "radio", {"spi": (18, 19, 16, 21), "busy": (10,), "dio1": (20,), "nreset": (6,)},
                  one_window=True,
                  properties={"rx_frequency": 865100000, "rx_sf": 10, "rx_bw": 125000, "rx_cr": 1, "rx_preamble": 8,
                              "rx_crc": True, "rx_iq_inverted": False, "rx_sync_word": 0x1424, "rx_rssi": -73,
                              "rx_snr": 700},
                  **checks)


# Puts one frame on air for the radio to receive, a little after the sample
# says it listens (it logs before it calls lora_recv).
def lora_frame(payload: bytes) -> str:
    return f"import time\ntime.sleep_ms(300)\nradio.write_region('rx', 0, {payload!r})\n"


ZEPHYR_PARTS: dict[str, tuple[Attach, ...]] = {
    # The bmp388 node of devices/bmp388/test/dut/rp2.overlay: I2C0 on GP4/GP5
    # at 0x76. Pressure in Pa, temperature in m°C (devices/bmp388/bmp388.c).
    "zephyr/sensor/pressure_polling": (
        Attach("bmp388", "baro", {"i2c": (4, 5, 0x76)}, {"i2c": 2}, {"pressure": 98765, "temperature": 23450}),
    ),
    # samples/drivers/lora/send/src/main.c:23-71: sends "helloworld 0" to
    # "helloworld 9" in turn, each MAX_DATA_LEN 12 bytes; the first after the
    # restart is the one the radio captures first once the queue is emptied.
    "zephyr/drivers/lora/send": (
        _sx1262(checks=(Check("bytes(part.read_region('tx', 0, part.tx_length)) if part.tx_pending else None",
                              lambda v: v == b"helloworld 0", "the first frame sent after the restart", 4.0,
                              setup="while part.tx_pending:\n    part.tx_next = True\n"),)),
    ),
    "zephyr/drivers/lora/receive": (_sx1262(),),
    # The neo_m8n node of devices/neo_m8n/test/dut/uart1/rp2.overlay: uart1,
    # DUT TX GP8 and RX GP9, so the part's TX (the binding's first pin) is
    # GP9. A 3D fix (fix=3) with 8 satellites, as test/pico_examples/parts.py
    # gives gps_uart.
    "zephyr/drivers/gnss": (
        Attach("neo_m8n", "gps", {"uart": (9, 8)}, {},
               {"latitude": 473977420, "longitude": -1223321500, "speed": 1500, "heading": 9000000,
                "satellites": 8, "fix": 3, "time": (2026, 11, 17, 12, 34, 56)}),
    ),
}
