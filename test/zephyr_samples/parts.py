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
    # The FT5336 at 0x38 on I2C0, a finger down at (120, 200) throughout.
    "zephyr/subsys/input/input_dump@ft5336": (
        Attach("ft5336", "touch", {"i2c": (4, 5, 0x38)}, {"i2c": 2}, {"position": (120, 200), "pressed": True}),
    ),
    # The SM351LT's output on GP15, closed (low) half of every 3 s: the
    # sample polls every 2 s, so its readings cycle through both levels.
    "zephyr/sensor/sm351lt": (Attach("square_wave", "magnet", {"out": (15,)},
                                     properties={"period": 3_000_000_000, "high": 1_500_000_000}),),
    # The Grove sensors' output on ADC input 0 at half of 3.3 V: the
    # thermistor at its 25 °C resistance, the light sensor's divider at
    # 12.6 lux by its driver's formula.
    **{f"zephyr/sensor/{s}": (adc_level("grove", 26, 500, True),) for s in ("grove_temperature", "light_polling")},
    # ADC input 0 (GP26) at 25 % of 3.3 V, input 1 (GP27) at 75 %.
    "zephyr/drivers/adc/adc_dt": (adc_level("ain0", 26, 250, False),),
    "zephyr/drivers/adc/adc_sequence": (adc_level("ain0", 26, 250, False), adc_level("ain1", 27, 750, True)),
    # The bmp388 node of devices/bmp388/test/dut/rp2.overlay: I2C0 on GP4/GP5
    # at 0x76. Pressure in Pa, temperature in m°C (devices/bmp388/bmp388.c).
    "zephyr/sensor/pressure_polling": (
        Attach("bmp388", "baro", {"i2c": (4, 5, 0x76)}, {"i2c": 2}, {"pressure": 98765, "temperature": 23450}),
    ),
    # pressure_interrupt: the same part, its INT pad on GP15.
    "zephyr/sensor/pressure_interrupt": (
        Attach("bmp388", "baro", {"i2c": (4, 5, 0x76), "int": (15,)}, {"i2c": 2},
               {"pressure": 98765, "temperature": 23450}),
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
    # The Grove LCD RGB Backlight on I2C0: the AIP31068 LCD at 0x3E, where
    # the sample writes a ten-digit counter from address 0. The driver sets
    # entry mode I/D 0 with shift (grove_lcd_rgb.c), so the address counter
    # runs down from 0 and the digits land at 00h and below it, wrapping
    # (HD44780U p. 26): ten of them in the DDRAM is the count. Its backlight's
    # PCA9633 at 0x62 shares the bus, and a harness I2C target answers one
    # address per bus (the RP2's I2C block has one target address), so it
    # goes unanswered and the driver's backlight writes fail.
    "zephyr/drivers/misc/grove_display": (
        Attach("aip31068", "lcd", {"i2c": (4, 5, 0x3e)}, {"i2c": 2}, checks=(
            Check("bytes(part.read_region('ddram', 0, 128))", lambda v: sum(48 <= b <= 57 for b in v) >= 10,
                  "the sample's ten-digit counter in the DDRAM", 3.0),
        )),
    ),
    # samples/drivers/led/pca9633: the part at 0x62 on I2C0, which the
    # sample turns on, off and to half brightness LED by LED.
    "zephyr/drivers/led/pca9633": (
        Attach("pca9633", "leds", {"i2c": (4, 5, 0x62)}, {"i2c": 2}, checks=(
            Check("part.get('writes')", lambda v: v >= 4, "the driver's LED writes", 5.0),
        )),
    ),
    # The MPU-9250 at 0x68 on I2C0, INT on GP15, its AK8963 at 30, -15 and
    # 45 uT: 200, -100 and 300 counts of 0.15 uT.
    **{f"zephyr/sensor/{sample}": (Attach("mpu9250", "imu", {"i2c": (4, 5, 0x68), "int": (15,)}, {"i2c": 2},
                                          {"magnetic_field": (30000, -15000, 45000)}),)
       for sample in ("magn_polling", "magn_trig")},
    # The DS3231 at 0x68 on I2C0, INT/SQW on GP15.
    "zephyr/drivers/counter/maxim_ds3231": (
        Attach("ds3231", "rtc", {"i2c": (4, 5, 0x68), "int": (15,)}, {"i2c": 2}, checks=(
            Check("part.get('alarms')[0]", lambda v: v >= 2, "both alarm-1 matches", 3.0),
        )),
    ),
    # The ZFM-20 on uart1 (its RX on the DUT's TX GP8), finger 1 on the
    # sensor throughout: the sample enrolls IDs 1 and 2 without waiting for
    # a lift, so both hold that finger.
    "zephyr/drivers/biometrics/fingerprint": (
        Attach("zfm20", "fingerprint", {"uart": (9, 8)}, {}, {"finger": 1, "score": 157}, checks=(
            Check("part.get('templates')", lambda v: v == 1, "template 2 left after the delete", 3.0),
        )),
    ),
    # The MCP4728 at 0x60 on I2C0: the sawtooth's 1 ms steps on channel A.
    "zephyr/drivers/dac@mcp4728": (
        Attach("mcp4728", "dac", {"i2c": (4, 5, 0x60)}, {"i2c": 2}, checks=(
            Check("part.get('writes')", lambda v: v > 300, "the sawtooth's steps written", 3.0),
            Check("part.get('codes')", lambda v: 0 < v[0] <= 4095 and list(v[1:]) == [0, 0, 0],
                  "channel A climbing, the rest at their EEPROM's 0", 3.0),
        )),
    ),
    # The AD5693R at 0x4C on I2C0: the driver's reset (read back as 0) and
    # configuration, then the 16-bit sawtooth's 1 ms steps.
    "zephyr/drivers/dac@ad5693r": (
        Attach("ad5693r", "dac", {"i2c": (4, 5, 0x4c)}, {"i2c": 2}, checks=(
            Check("part.get('writes')", lambda v: v > 300, "the sawtooth's steps written", 3.0),
            Check("part.get('code')", lambda v: 0 < v < 65536, "the output climbing", 3.0),
        )),
    ),
    # The HTS221 at 0x5F on I2C0 at 23.5 C and 47.5 %RH, values its
    # coefficients represent exactly.
    "zephyr/sensor/hts221": (
        Attach("hts221", "rh", {"i2c": (4, 5, 0x5f)}, {"i2c": 2}, {"temperature": 23500, "humidity": 47500}),
    ),
    # The LSM303DLHC's magnetometer at 0x1E on I2C0: 0.25, -0.1 and 0.5
    # gauss, whole counts at the driver's default +-1.3 gauss gain.
    "zephyr/sensor/magn_polling@lsm303dlhc": (
        Attach("lsm303dlhc_magn", "magn", {"i2c": (4, 5, 0x1e)}, {"i2c": 2},
               {"magnetic_field": (25000, -10000, 50000)}),
    ),
    # The DRV2605L at 0x5A on I2C0: the sample's sequence, effects 1 to 4
    # with 100 ms waits, played whole on GO at each of the run's DUT boots.
    "zephyr/drivers/haptics/drv2605": (
        Attach("drv2605l", "haptic", {"i2c": (4, 5, 0x5a)}, {"i2c": 2}, checks=(
            Check("(part.get('played'), part.get('last'), part.get('playing'))",
                  lambda v: v[0] >= 4 and v[0] % 4 == 0 and v[1] == 4 and not v[2],
                  "effects 1-4 played whole and done", 3.0),
        )),
    ),
    # The LP5012 at 0x14 on I2C0: enabled, and the channel test's first
    # color, red, on all four modules for its first seconds.
    "zephyr/drivers/led/lp50xx": (
        Attach("lp5012", "leds", {"i2c": (4, 5, 0x14)}, {"i2c": 2}, checks=(
            Check("part.get('enabled')", lambda v: v is True or v == 1, "the chip on", 3.0),
            *(Check(f"part.get('led{i}')", lambda v: list(v) == [255, 0, 0], f"module {i} red", 3.0)
              for i in range(4)),
        )),
    ),
    # The IS31FL3216A at 0x74 on I2C0: out of shutdown, and the sample's
    # fades loaded into the outputs update by update.
    "zephyr/drivers/led/is31fl3216a": (
        Attach("is31fl3216a", "leds", {"i2c": (4, 5, 0x74)}, {"i2c": 2}, checks=(
            Check("part.get('on')", lambda v: v is True or v == 1, "out of software shutdown", 3.0),
            Check("part.get('updates')", lambda v: v >= 20, "the fade's PWM loaded update by update", 3.0),
        )),
    ),
    # The SX1509B at 0x3E on I2C0: the LED driver on I/O 5-7, the sample's
    # three LEDs.
    "zephyr/drivers/led/sx1509b_intensity": (
        Attach("sx1509b", "expander", {"i2c": (4, 5, 0x3e)}, {"i2c": 2}, checks=(
            Check("part.get('led_driver')", lambda v: v & 0xe0 == 0xe0, "LED drivers on I/O 5-7", 3.0),
        )),
    ),
    # The VL53L0X at 0x29 on I2C0, 200 mm away.
    "zephyr/sensor/vl53l0x": (
        Attach("vl53l0x", "tof", {"i2c": (4, 5, 0x29)}, {"i2c": 2}, {"distance": 200}, checks=(
            Check("part.rangings", lambda v: v > 1, "the sample's rangings", 2.0),
        )),
    ),
    # The SCT2024 on SPI0: the sample turns LED 0 on first, then one more
    # each 500 ms, each write latched with an LA/ pulse.
    "zephyr/drivers/led/sct2024": (
        Attach("sct2024", "leds", {"spi": (18, 19, 17), "la": (20,), "oe": (21,)}, {}, checks=(
            Check("(part.outputs & 1, part.enabled, part.latches > 1)", lambda v: v == (1, True, True),
                  "LED 0 on, the outputs enabled, and the writes latched (OUT0, enabled, latched)", 3.0),
        )),
    ),
    # The IS31FL3194 at 0x53 on I2C0: out of shutdown, and the sample's fades
    # latched by color updates.
    "zephyr/drivers/led/is31fl319x": (
        Attach("is31fl3194", "rgb", {"i2c": (4, 5, 0x53)}, {"i2c": 2}, checks=(
            Check("(part.configuration & 1, part.updates > 0, any(part.levels))",
                  lambda v: v == (1, True, True),
                  "out of shutdown, with color updates that lit a channel (SSD, updated, lit)", 3.0),
        )),
    ),
    # The IS31FL3733 at 0x50 on I2C0: out of shutdown at full current, every
    # dot switched on, and the sample's PWM writes in page 1.
    "zephyr/drivers/led/is31fl3733": (
        Attach("is31fl3733", "matrix", {"i2c": (4, 5, 0x50)}, {"i2c": 2}, checks=(
            Check("(part.configuration & 1, part.gcc, part.leds_on, part.pwm_max)",
                  lambda v: v[0] == 1 and v[1] in (0xFF, 0x3F) and v[2] == 192 and v[3] != 0,
                  "out of shutdown, every dot on, and some PWM set (SSD, GCC, dots on, highest PWM)", 3.0),
        )),
    ),
    # The LP3943 at 0x60 on I2C0: the sample turns its LEDs on one by one.
    "zephyr/drivers/led/lp3943": (
        Attach("lp3943", "leds", {"i2c": (4, 5, 0x60)}, {"i2c": 2}, checks=(
            Check("part.get('selectors')", lambda v: v != 0, "LEDs switched on", 2.0),
        )),
    ),
    # The LP5562 at 0x30 on I2C0: enabled, and the sample's colours and
    # blinks written.
    "zephyr/drivers/led/lp5562": (
        Attach("lp5562", "leds", {"i2c": (4, 5, 0x30)}, {"i2c": 2}, checks=(
            Check("part.get('enabled')", lambda v: v is True or v == 1, "the chip on", 3.0),
            Check("part.get('writes')", lambda v: v >= 5, "the LEDs written", 3.0),
        )),
    ),
    # The LP5569 at 0x32 on I2C0: enabled, and the sample's LEDs driven.
    "zephyr/drivers/led/lp5569": (
        Attach("lp5569", "leds", {"i2c": (4, 5, 0x32)}, {"i2c": 2}, checks=(
            Check("part.get('enabled')", lambda v: v is True or v == 1, "the chip on", 3.0),
            Check("part.get('writes')", lambda v: v >= 3, "the LEDs written", 3.0),
        )),
    ),
    # The AT24C02C at 0x50 on I2C0, erased at attach: the sample writes its
    # boot counter at offset 0.
    "zephyr/drivers/eeprom": (
        Attach("at24c02", "eeprom", {"i2c": (4, 5, 0x50)}, {"i2c": 2}, checks=(
            Check("part.get('writes')", lambda v: v > 0, "the sample's boot counter written", 3.0),
        )),
    ),
    # The MAX6675 on SPI0 (SCK GP18, MOSI GP19, MISO GP16, CS GP17), its
    # thermocouple at 123.25 C.
    "zephyr/sensor/max6675": (
        Attach("max6675", "tc", {"spi": (18, 19, 16, 17)}, {}, {"temperature": 123250}),
    ),
    # The LSM6DSL at 0x6A on I2C0, INT1 on GP15, at rest: 1 g on z.
    "zephyr/sensor/lsm6dsl": (
        Attach("lsm6dsl", "imu", {"i2c": (4, 5, 0x6a), "int1": (15,)}, {"i2c": 2},
               {"acceleration": (0, 0, 1000), "angular_rate": (0, 0, 0)}),
    ),
    # The LSM6DSO at 0x6A on I2C0, INT1 on GP15, at rest: 1 g on z.
    "zephyr/sensor/lsm6dso": (
        Attach("lsm6dso", "imu", {"i2c": (4, 5, 0x6a), "int1": (15,)}, {"i2c": 2},
               {"acceleration": (0, 0, 1000), "angular_rate": (0, 0, 0)}),
    ),
    # The LPS22HB at 0x5C on I2C0, at 100 kPa and 23.5 C.
    "zephyr/sensor/lps22hb": (
        Attach("lps22hb", "baro", {"i2c": (4, 5, 0x5c)}, {"i2c": 2}, {"pressure": 100000, "temperature": 23500}),
    ),
    # Vishay's light sensors at 0x29 on I2C0, every channel at 1000 counts.
    **{f"zephyr/sensor/{model}": (Attach(model, "light", {"i2c": (4, 5, 0x29)}, {"i2c": 2},
                                         {"counts": (1000, 1000, 1000, 1000)}),)
       for model in ("veml6031", "veml6046")},
    # Sensirion's humidity sensors at 0x44 on I2C0, at 23.5 C and 45 %RH.
    **{f"zephyr/sensor/{sample}": (Attach(model, "rh", {"i2c": (4, 5, 0x44)}, {"i2c": 2},
                                          {"temperature": 23500, "humidity": 45000}),)
       for sample, model in (("sht3xd", "sht3x"), ("sht4x", "sht4x"))},
    # TI's temperature sensors at 0x48 on I2C0 (GP4/GP5), at 23.5 C.
    **{f"zephyr/sensor/{sample}": (Attach(model, "thermo", {"i2c": (4, 5, 0x48)}, {"i2c": 2}, {"temperature": 23500}),)
       for sample, model in (("tmp108", "tmp108"), ("tmp112", "tmp112"), ("tmp11x", "tmp116"))},
    # The CAN samples: the MCP2515 on SPI0 (SCK GP18, MOSI GP19, MISO GP16,
    # CS GP17), INT on GP15.
    **{f"zephyr/{sample}": (Attach("mcp2515", "can", {"spi": (18, 19, 16, 17), "int": (15,)}),)
       for sample in ("drivers/can/counter", "drivers/can/babbling", "subsys/canbus/isotp", "net/sockets/can")},
    # spi_flash: the W25Q16JV, answering Read JEDEC ID, which Zephyr's
    # spi-nor driver checks at boot, and holding the sector the sample
    # erases and writes at 0xff000.
    "zephyr/drivers/spi_flash": (
        Attach("w25q16jv", "flash", {"spi": (18, 19, 16, 17)}, {}, {"id_read": True, "held_base": 0xff000}),
    ),
    # spi_flash_at45: the AT45DB041E, holding the sample's test region
    # (0xfe00, 1 KiB) in the pages from 0xfe00 on. The driver writes by
    # Read-Modify-Write (CONFIG_SPI_FLASH_AT45_USE_READ_MODIFY_WRITE, on by
    # default), a page at a time, so the sample erases nothing; it ends in
    # Deep Power-Down.
    "zephyr/drivers/spi_flash_at45": (
        Attach("at45db041e", "flash", {"spi": (18, 19, 16, 17)}, {}, {"held_base": 0xfe00}, checks=(
            Check("(part.power_down,) + tuple(now - then for now, then in zip("
                  "(part.programs, part.lost, part.ignored), at45_at_restart))",
                  lambda v: v[0] is True and v[1] >= 4 and v[2:] == (0, 0),
                  "the test region's four pages written, nothing lost or ignored, and left in Deep Power-Down "
                  "(power_down, programs, lost, ignored)", 2.0,
                  setup="at45_at_restart = (part.programs, part.lost, part.ignored)\n",
                  before_restart=True),
        )),
    ),
    # accel_trig: the same part, its INT pin on GP15.
    "zephyr/sensor/accel_trig": (
        Attach("mpu6050", "imu", {"i2c": (4, 5, 0x68), "int": (15,)}, {"i2c": 2},
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
    # drain with no pull-up of its own; the overlay turns on the DUT pad's
    # pull-up (GPIO_PULL_UP), where pico-examples' gpio_init leaves the
    # pad's pull-down on, and that alone carries the bus on fixture 1's
    # bare lines: both samples passed there twice each.
    **{f"zephyr/{target}": (Attach("ds18b20", "thermo", {"dq": (15,)},
                                   properties={"temperature": 21375, "serial": 0xC0FFEE}),)
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
    # The cfb and LVGL samples draw on the same SSD1306.
    **{f"zephyr/{s}": (
        Attach("ssd1306", "oled", {"i2c": (4, 5, 0x3C)}, {"i2c": 2}, checks=(
            Check("(part.display_on, any(part.read_region('gddram', 0, 1024)))", lambda v: v == (True, True),
                  "the display on, with text drawn in its GDDRAM", 3.0),
        )),
    ) for s in ("subsys/display/cfb", "subsys/display/cfb_shell", "subsys/display/cfb_custom_font",
                "subsys/display/lvgl", "modules/lvgl/demos")},
    # screen_transparency: the WS2812 chain under the 32 x 40 LED matrix
    # (overlays/led_matrix_32x40.overlay), 1280 LEDs, 3840 bytes a frame.
    # The model keeps the first 256, rows 0-7, where the sample's "Hello,
    # world!" (0xff00ff, 20 px above the centre in a 14 px font) lies; the
    # transparent screen leaves the rest dark. A magenta LED there is green
    # 0 with red and blue equal.
    "zephyr/modules/lvgl/screen_transparency": (
        Attach("ws2812b", "strip", {"din": (6,)}, properties={"count": 256}, checks=(
            Check("(part.bad_frames - globals().get('_m_bad', 0), part.length, (lambda b: any("
                  "b[i] == 0 and b[i + 1] > 0 and b[i + 1] == b[i + 2] for i in range(0, 768, 3)))"
                  "(part.read_region('leds', 0, 768)))",
                  lambda v: v == (0, 3840, True),
                  "whole 1280-LED frames, none out of the datasheet's windows, magenta text in rows 0-7", 4.0,
                  setup="import time\n_f = part.frames\nwhile part.frames < _f + 2:\n    time.sleep_ms(1)\n"
                        "_m_bad = part.bad_frames\n"),
        )),
    ),
    # multi_display: the same SSD1306, and a second on I2C1.
    "zephyr/modules/lvgl/multi_display": tuple(
        Attach("ssd1306", name, {"i2c": (sda, scl, 0x3C)}, {"i2c": 2}, checks=(
            Check("(part.display_on, any(part.read_region('gddram', 0, 1024)))", lambda v: v == (True, True),
                  f"{name}: the display on, with a demo drawn in its GDDRAM", 3.0),
        )) for name, sda, scl in (("oled", 4, 5), ("oled2", 6, 7))),
    # accelerometer_chart: the same SSD1306, and the LIS2DH on I2C1.
    "zephyr/modules/lvgl/accelerometer_chart": (
        Attach("ssd1306", "oled", {"i2c": (4, 5, 0x3C)}, {"i2c": 2}, checks=(
            Check("(part.display_on, any(part.read_region('gddram', 0, 1024)))", lambda v: v == (True, True),
                  "the display on, with the chart drawn in its GDDRAM", 3.0),
        )),
        Attach("lis3dh", "accel", {"i2c": (6, 7, 0x18)}, {"i2c": 2}, {"acceleration": (252, -500, 1020)}),
    ),
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
    # INA219 under the composite gauge: a 3.7 V cell, 123.4 mA.
    "zephyr/drivers/fuel_gauge": (
        Attach("ina219", "power", {"i2c": (4, 5, 0x40)}, {"i2c": 2},
               {"bus_voltage": 3700, "current": 123400, "shunt_resistance": 100000}),
    ),
    # The LCD backpack at 0x27: the sample's text in the HD44780's DDRAM.
    "zephyr/drivers/auxdisplay": (
        Attach("hd44780_pcf8574", "lcd", {"i2c": (4, 5, 0x27)}, {"i2c": 2}, checks=(
            Check("bytes(part.read_region('ddram', 0, 128))", lambda v: b"Hello world from" in v,
                  "the sample's greeting in the display's DDRAM", 3.0),
        )),
    ),
    # The sensing sample's base (MPU-6050, I2C0) and lid (ICM-42688, SPI0:
    # SCK GP18, MOSI GP19, MISO GP16, CS GP17) sensors.
    "zephyr/subsys/sensing/simple": (
        Attach("mpu6050", "base", {"i2c": (4, 5, 0x68)}, {"i2c": 2},
               {"acceleration": (250, -500, 1125), "angular_rate": (10000, -2000, 3000), "temperature": 26530}),
        Attach("icm42688", "lid", {"spi": (18, 19, 16, 17)}),
    ),
    # cellular_modem: an A7672X on uart1 (DUT TX GP8, RX GP9), PWRKEY on
    # GP6, its LCP asking for a 1500-byte MRU: the sample sends
    # 1024-byte datagrams, and Zephyr's PPP takes the peer's MRU as its MTU
    # (subsys/net/l2/ppp/lcp.c) with no IPv4 fragmentation.
    "zephyr/net/cellular_modem": (
        Attach("a7672x", "modem", {"uart": (9, 8), "pwrkey": (6,)}, {}, {"mru": 1500}),
    ),
    # 6dof_motion_drdy: the ICM-42688 on SPI0 (SCK GP18, MOSI GP19, MISO
    # GP16, CS GP17), INT1 on GP15. Raw counts, as devices/icm4268x's README.
    "zephyr/sensor/6dof_motion_drdy": (
        Attach("icm42688", "imu", {"spi": (18, 19, 16, 17), "int1": (15,)}, {},
               {"accel": (2048, -2048, 1024), "gyro": (164, -164, 8200), "temperature": 1325}),
    ),
    # MCP9808: temperature in m°C, a multiple of its 0.0625 °C step.
    **{f"zephyr/sensor/{sample}": (
        Attach("mcp9808", "thermo", {"i2c": (4, 5, 0x18)}, {"i2c": 2}, {"temperature": 25250}),
    ) for sample in ("thermometer", "jc42")},
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
