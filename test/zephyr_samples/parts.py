# SPDX-License-Identifier: Apache-2.0 OR MIT
# The Metalclaw parts an upstream Zephyr sample talks to, attached before it
# starts, as for a pico-examples program (test/pico_examples/parts.py): each
# binding names DUT pins, here the pins of the devicetree nodes the sample
# is built with (test/zephyr_samples/builds.py). Property values are chosen
# unlike the models' defaults, so what the sample prints shows it came from
# the part.

from test.pico_examples.parts import Attach, Check, _i2c_jumpers, _ws2812_frames


# The sx1262 node of devices/sx126x/test/dut/rp2.overlay: SPI0 SCK GP18, MOSI
# GP19, MISO GP16, CS GP21 (a GPIO), NRESET GP6, BUSY GP10, DIO1 GP20. The
# radio's SPI engine drives BUSY and DIO1, so those and the SPI pins come
# from one PIO window (on the carrier, BUSY takes the GPIO22 branch); NRESET
# is a plain input and takes its own. What it receives is set to the
# lora samples' channel (samples/drivers/lora/*/src/main.c: 865.1 MHz,
# SF10, 125 kHz, CR 4/5, preamble 8, private network).
def _sx1262(**checks) -> Attach:
    return Attach("sx1262", "radio", {"spi": (18, 19, 16, 21), "busy": (10,), "dio1": (20,), "nreset": (6,)},
                  one_window=("spi", "busy", "dio1"),
                  properties={"rx_frequency": 865100000, "rx_sf": 10, "rx_bw": 125000, "rx_cr": 1, "rx_preamble": 8,
                              "rx_crc": True, "rx_iq_inverted": False, "rx_sync_word": 0x1424},
                  **checks)


# Every frame the radio has captured, oldest first, emptying its queue: a
# Check's setup defines it, so its expression can call it. The queue is not
# emptied at the restart, which may come after the sample's first frame.
TX_FRAMES = ("def _tx_frames(radio):\n"
             "    out = []\n"
             "    while radio.tx_pending:\n"
             "        out.append(bytes(radio.read_region('tx', 0, radio.tx_length)))\n"
             "        radio.tx_next = True\n"
             "    return out\n")


def tx_frames(test, description: str, after_seconds: float) -> Check:
    return Check("_tx_frames(part)", test, description, after_seconds, setup=TX_FRAMES)


# Puts one frame on air for the radio to receive, a little after the sample
# says it listens (it logs before it calls lora_recv). The model takes a
# frame's signal as set for that frame alone, so it is set with each, as
# the rig's radio cases do (devices/sx126x/test/cases.py Radio.inject).
def lora_frame(payload: bytes) -> str:
    return (f"import time\ntime.sleep_ms(300)\nradio.rx_rssi = -73\nradio.rx_snr = 700\n"
            f"radio.write_region('rx', 0, {payload!r})\n")


# A level on an ADC input: the carrier's RC filters on GP26-28 turn the
# harness's 1 MHz square wave into its duty share of 3.3 V; a bench without
# them (fixture "analog_pins") holds the pin low or high instead.
def adc_level(name: str, pin: int, high_ns: int, level: bool) -> Attach:
    return Attach("square_wave", name, {"out": (pin,)}, properties={"period": 1000, "high": high_ns},
                  without_analog={"period": 0, "high": 0, "level": level})


ZEPHYR_PARTS: dict[str, tuple[Attach, ...]] = {
    # input_dump's button (pressed 100 ms a second) and wheel: the
    # harness's encoder, 100 steps a second back and forth over 2000, well
    # inside the qdec's 2 ms sampling.
    "zephyr/subsys/input/input_dump": (
        Attach("square_wave", "button", {"out": (15,)}, properties={"period": 1_000_000_000, "high": 900_000_000}),
        Attach("quadrature_encoder", "encoder", {"ab": (10, 11)}, properties={"travel": 2000, "rate": 100}),
    ),
    # The SM351LT's output on GP15, closed (low) half of every 3 s: the
    # sample polls every 2 s, so its readings cycle through both levels.
    "zephyr/sensor/sm351lt": (Attach("square_wave", "magnet", {"out": (15,)},
                                     properties={"period": 3_000_000_000, "high": 1_500_000_000}),),
    # ADC input 0 (GP26) at 25 % of 3.3 V, input 1 (GP27) at 75 %.
    "zephyr/drivers/adc/adc_dt": (adc_level("ain0", 26, 250, False),),
    "zephyr/drivers/adc/adc_sequence": (adc_level("ain0", 26, 250, False), adc_level("ain1", 27, 750, True)),
    # The bmp388 node of devices/bmp388/test/dut/rp2.overlay: I2C0 on GP4/GP5
    # at 0x76. Pressure in Pa, temperature in m°C (devices/bmp388/bmp388.c).
    "zephyr/sensor/pressure_polling": (
        Attach("bmp388", "baro", {"i2c": (4, 5, 0x76)}, {"i2c": 2}, {"pressure": 98765, "temperature": 23450}),
    ),
    # samples/drivers/lora/send/src/main.c:23-71: sends "helloworld 0" to
    # "helloworld 9" in turn, each MAX_DATA_LEN 12 bytes, a second apart.
    "zephyr/drivers/lora/send": (
        _sx1262(checks=(tx_frames(lambda v: b"helloworld 0" in v and all(f[:11] == b"helloworld " and len(f) == 12
                                                                         for f in v),
                                  "the helloworld frames the sample sent", 4.0),)),
    ),
    # samples/subsys/lorawan/class_a/src/main.c:95: joins over OTAA, which
    # puts a JoinRequest on air: MHDR 0x00 (join request), AppEUI, DevEUI and
    # DevNonce, MIC: 23 bytes (LoRaWAN 1.0.x 6.2.4). No network answers.
    "zephyr/subsys/lorawan/class_a": (
        _sx1262(checks=(tx_frames(lambda v: any(len(f) == 23 and f[0] == 0x00 for f in v),
                                  "a JoinRequest", 6.0),)),
    ),
    "zephyr/drivers/lora/receive": (_sx1262(),),
    # samples/drivers/modem/at_client on uart1 (DUT TX GP8, RX GP9), so the
    # part's TX is GP9. The sample never presses PWRKEY, which the model's
    # pin would need to power on (devices/cellular/modem.h): unbound, the
    # part is powered by setting `powered`, as a person would press the key.
    "zephyr/drivers/modem/at_client": (
        Attach("sim7000", "modem", {"uart": (9, 8)}, {}, {"powered": True}),
    ),
    # Wave 1e, on I2C0 at GP4/GP5 (overlays/i2c0.overlay), with the values
    # test/pico_examples/parts.py gives the same models. MPU-6050:
    # acceleration in mg, angular rate in mdps, temperature in m°C.
    "zephyr/sensor/mpu6050": (
        Attach("mpu6050", "imu", {"i2c": (4, 5, 0x68)}, {"i2c": 2},
               {"acceleration": (250, -500, 1125), "angular_rate": (10000, -2000, 3000), "temperature": 26530}),
    ),
    # LIS3DH (the sample's LIS2DH driver): acceleration in mg.
    "zephyr/sensor/lis2dh": (
        Attach("lis3dh", "accel", {"i2c": (4, 5, 0x18)}, {"i2c": 2}, {"acceleration": (252, -500, 1020)}),
    ),
    # INA219: bus voltage in mV, current in uA, the 100 mohm shunt in uohm.
    "zephyr/sensor/ina219": (
        Attach("ina219", "power", {"i2c": (4, 5, 0x40)}, {"i2c": 2},
               {"bus_voltage": 5124, "current": 123400, "shunt_resistance": 100000}),
    ),
    "zephyr/sensor/accel_polling": (
        Attach("lis3dh", "accel", {"i2c": (4, 5, 0x18)}, {"i2c": 2}, {"acceleration": (252, -500, 1020)}),
    ),
    # BME280 on the sample's rpi_pico SPI0: spi=(SCK, MOSI, MISO, CS); the
    # Zephyr driver clocks SPI mode 3 (drivers/sensor/bosch/bme280/bme280.h).
    # Pressure in Pa, temperature in m°C, humidity in 0.001 %RH.
    "zephyr/sensor/bme280": (
        Attach("bme280", "env", {"spi": (18, 19, 16, 17)}, {},
               {"spi_mode": 3, "pressure": 99887, "temperature": 21370, "humidity": 45670}),
    ),
    # sw0 on GP15 pressed for 100 ms each second: the harness's square_wave
    # (period and high time in ns) holds the line high and pulls it low, as
    # a button to ground would.
    **{f"zephyr/{target}": (Attach("square_wave", "button", {"out": (15,)},
                                   properties={"period": 1_000_000_000, "high": 900_000_000}),)
       for target in ("basic/button", "drivers/gpio/button_interrupt")},
    # The stepper sample's mode button, pressed for 100 ms each second.
    "zephyr/drivers/stepper/generic": (Attach("square_wave", "button", {"out": (15,)},
                                              properties={"period": 1_000_000_000, "high": 900_000_000}),),
    # DS18B20 on GP15: 21.375 °C is 342 sixteenths, exact at 12 bits. Open
    # drain with no pull-up of its own, so only the carrier's lines carry it.
    **{f"zephyr/{target}": (Attach("ds18b20", "thermo", {"dq": (15,)},
                                   properties={"temperature": 21375, "serial": 0xC0FFEE}, needs_line_pullups=True),)
       for target in ("sensor/ds18b20", "drivers/w1/scanner")},
    # HT16K33 at 0x70. samples/drivers/ht16k33/src/main.c:26-31 turns its 128
    # LEDs on one by one, 100 ms apart, from LED 0; the driver keeps LED n in
    # display byte n / 8, bit n % 8 (drivers/led/ht16k33.c). By 8 s in the
    # first five bytes are full, with the oscillator and display on.
    "zephyr/drivers/ht16k33": (
        Attach("ht16k33", "leds", {"i2c": (4, 5, 0x70)}, {"i2c": 2}, checks=(
            Check("(part.oscillator, part.display_on, bytes(part.read_region('display', 0, 16)))",
                  lambda v: v[:2] == (True, True) and v[2][:5] == b"\xff" * 5,
                  "the oscillator and display on, and LEDs 0-39 lit one by one", 8.0),
        )),
    ),
    # SSD1306 at 0x3C: samples/drivers/display draws its corner rectangles
    # and turns blanking off (src/display.c:400-457).
    "zephyr/drivers/display": (
        Attach("ssd1306", "oled", {"i2c": (4, 5, 0x3C)}, {"i2c": 2}, checks=(
            Check("(part.display_on, any(part.read_region('gddram', 0, 1024)))", lambda v: v == (True, True),
                  "the display on, with something drawn in its GDDRAM", 3.0),
        )),
    ),
    # spi_bitbang's loopback: the harness carries MOSI (GP12) to MISO (GP13).
    "zephyr/drivers/spi_bitbang": (
        Attach("jumper", "mosi_miso", {"source": (12,), "sink": (13,)}, one_window=True),
    ),
    # uart/passthrough's loop: the harness carries uart1 TX (GP8) to RX (GP9).
    "zephyr/drivers/uart/passthrough": (
        Attach("jumper", "uart1_loop", {"source": (8,), "sink": (9,)}, one_window=True),
    ),
    # A 16-pixel WS2812 strip on GP6: led_strip updates it every
    # CONFIG_SAMPLE_LED_UPDATE_DELAY (50 ms by default); 16 x 3 bytes a frame.
    "zephyr/drivers/led/led_strip": (
        Attach("ws2812b", "strip", {"din": (6,)}, properties={"count": 16}, checks=(
            _ws2812_frames("strip", 48, "frames of 48 bytes, none with a pulse out of the datasheet's windows"),
        )),
    ),
    # rtio_loopback's bus: open-drain jumpers GP4 <-> GP6 (SDA) and GP5 <->
    # GP7 (SCL), as pico-examples' slave_mem_i2c is wired.
    "zephyr/drivers/i2c/rtio_loopback": _i2c_jumpers(),
    # The cfb samples draw text on the same SSD1306.
    **{f"zephyr/subsys/display/{s}": (
        Attach("ssd1306", "oled", {"i2c": (4, 5, 0x3C)}, {"i2c": 2}, checks=(
            Check("(part.display_on, any(part.read_region('gddram', 0, 1024)))", lambda v: v == (True, True),
                  "the display on, with text drawn in its GDDRAM", 3.0),
        )),
    ) for s in ("cfb", "cfb_custom_font")},
    # BME280 at 0x76 on I2C0 as dht_polling's dht0: humidity in m%RH.
    "zephyr/sensor/dht_polling": (
        Attach("bme280", "env", {"i2c": (4, 5, 0x76)}, {"i2c": 2},
               {"pressure": 99887, "temperature": 21370, "humidity": 45670}),
    ),
    # INA237 at 0x40: bus voltage in mV, current in uA, the 15 mohm shunt.
    "zephyr/sensor/sensor_shell": (
        Attach("ina237", "power", {"i2c": (4, 5, 0x40)}, {"i2c": 2},
               {"bus_voltage": 3300, "current": 200000, "shunt_resistance": 15000, "temperature": 31250}),
    ),
    # MCP9808: temperature in m°C, a multiple of its 0.0625 °C step.
    "zephyr/sensor/thermometer": (
        Attach("mcp9808", "thermo", {"i2c": (4, 5, 0x18)}, {"i2c": 2}, {"temperature": 25250}),
    ),
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
