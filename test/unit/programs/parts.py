# SPDX-License-Identifier: Apache-2.0 OR MIT
# The parts the pico-examples programs the model checks transcribe
# (pico-examples 0d62f75) talk to: the model, its bindings at the program's
# default pins and the properties the checks set, and what the part must
# have seen of the program (Check). part_targets.py holds what the program
# prints with these.

from test.unit.programs import displays, lights
from test.zephyr_samples.attach import FLASH_AT_RESTART, FLASH_SINCE_RESTART, Attach, Check, ws2812_frames

# The parts each needs_part target uses, filled in as their models land.
# Bindings are the programs' default pins (PICO_DEFAULT_I2C_SDA_PIN 4,
# PICO_DEFAULT_I2C_SCL_PIN 5; PICO_DEFAULT_SPI_RX_PIN 16, _CSN_PIN 17,
# _SCK_PIN 18, _TX_PIN 19) and addresses; the property values are chosen to
# be unlike the models' defaults and away from %f rounding ties, so a
# reading shows it came from the rig.
def _i2c_part(model: str, address: int, properties: dict, **gpio) -> tuple[Attach, ...]:
    bindings = {"i2c": (4, 5, address), **{name: (pin,) for name, pin in gpio.items()}}
    return (Attach(model, model, bindings, {"i2c": 2}, properties),)


PARTS: dict[str, tuple[Attach, ...]] = {
    # i2c/bmp280_i2c/bmp280_i2c.c: ADDR 0x76.
    "bmp280_i2c": (
        Attach("bmp280", "baro", {"i2c": (4, 5, 0x76)}, {"i2c": 2},
               {"pressure": 98765, "temperature": 23450}),
    ),
    # i2c/mcp9808_i2c/mcp9808_i2c.c: ADDRESS 0x18. 34.562 degC is above the
    # 30 degC upper limit the program writes and under its other two. Its
    # limits (:70-99) are 0x01E0, 0x0140 and 0x0280 at 1/16 degC, which
    # the part keeps as TUPPER, TLOWER and TCRIT; at power-on all three are
    # 0.
    "mcp9808_i2c": (
        Attach("mcp9808", "thermo", {"i2c": (4, 5, 0x18)}, {"i2c": 2}, {"temperature": 34562},
               checks=(Check("(part.upper, part.lower, part.critical)", lambda v: tuple(v) == (30000, 20000, 40000),
                             "the program's 30, 20 and 40 degC limits in TUPPER, TLOWER and TCRIT"),)),
    ),
    # i2c/pcf8523_i2c/pcf8523_i2c.c: addr 0x68. The program writes its own
    # time over this one, which shows its writes land: the software reset
    # (:28-32), then 0 to every time register (:35-57), which clears OS
    # (Seconds bit 7) and leaves the counters running. 2 s after the
    # restart the part reads year, month, day, weekday, hour and minute 0
    # and a second or two, as the program itself prints them.
    "pcf8523_i2c": (
        Attach("pcf8523", "rtc", {"i2c": (4, 5, 0x68)}, {"i2c": 2},
               {"time": (26, 11, 15, 0, 12, 34, 56)},
               checks=(Check("(tuple(part.time), part.os, part.stop)",
                             lambda v: tuple(v[0][:6]) == (0,) * 6 and v[0][6] in (1, 2) and not v[1] and not v[2],
                             "the program's all-zero time, counting, its integrity flag cleared", 2.0),)),
    ),
    # spi/bme280_spi/bme280_spi.c: spi=(SCK, MOSI = TX, MISO = RX, CSn).
    "bme280_spi": (
        Attach("bme280", "env", {"spi": (18, 19, 16, 17)}, {},
               {"pressure": 99887, "temperature": 21370, "humidity": 45670}),
    ),
    # TI INA2xx, InvenSense MPU-6050, ST LIS3DH, NXP MMA8451Q and MPL3115A2:
    # i2c_default at the address each source names.
    # ina219_i2c.c:7, 21-23: 0x40, calibrated for a 0.1 ohm shunt.
    "ina219_i2c": _i2c_part("ina219", 0x40, {"bus_voltage": 5124, "current": 123400, "shunt_resistance": 100000}),
    # ina228_i2c.c:23, 27: 0x40, the stock 15 mohm shunt.
    "ina228_i2c": _i2c_part("ina228", 0x40, {"bus_voltage": 4850, "current": 92500, "shunt_resistance": 15000,
                                         "temperature": 41000}),
    # ina237_i2c.c:12, 26: 0x40, a 15 mohm shunt.
    "ina237_i2c": _i2c_part("ina237", 0x40, {"bus_voltage": 3300, "current": 200000, "shunt_resistance": 15000,
                                         "temperature": 31250}),
    # ina260_i2c.c:11: 0x40; the shunt is inside the part.
    "ina260_i2c": _i2c_part("ina260", 0x40, {"bus_voltage": 5000, "current": 742500}),
    # mpu6050_i2c.c:34: 0x68.
    "mpu6050_i2c": _i2c_part("mpu6050", 0x68, {"acceleration": (250, -500, 1125),
                                           "angular_rate": (10000, -2000, 3000), "temperature": 26530}),
    # lis3dh_i2c.c:27: 0x18.
    "lis3dh_i2c": _i2c_part("lis3dh", 0x18, {"acceleration": (252, -500, 1020), "temperature": 28750}),
    # mma8451_i2c.c:26: 0x1D.
    "mma8451_i2c": _i2c_part("mma8451q", 0x1D, {"acceleration": (500, -250, 2000)}),
    # mpl3115a2_i2c.c:28-29: 0x60, INT1 on GP16.
    "mpl3115a2_i2c": _i2c_part("mpl3115a2", 0x60, {"pressure": 95000, "altitude": 321500, "temperature": 21625},
                           int1=16),
    # Displays: what a check reads is the controller's memory and the
    # settings the program sent, judged by displays.py.
    # i2c/ssd1306_i2c/ssd1306_i2c.c:41-44, 330-376: 0x3C, a 128 x 32 panel. The
    # raspberries are drawn about 3.1 s in (three 1 s flashes first) and
    # scroll for 5 s; the text comes after.
    "ssd1306_i2c": (
        Attach("ssd1306", "oled", {"i2c": (4, 5, 0x3C)}, {"i2c": 2}, checks=(
            Check("part.read_region('gddram', 0, 1024)", lambda v: v == displays.ssd1306_raspberries(),
                  "the three raspberries at columns 0, 31 and 62 of pages 0-3, nothing else", 5.5),
            Check("(part.display_on, part.addressing_mode, part.multiplex, part.contrast, part.segment_remap, "
                  "part.com_remap, part.com_pins, part.charge_pump, part.entire_on, part.inverse, part.scroll, "
                  "part.column_range, part.page_range)",
                  lambda v: v == (True, 0, 32, 0xFF, True, True, 0x02, True, False, False, True, (62, 87), (0, 3)),
                  "SSD1306_init()'s settings, scrolling, and the last raspberry's render area", 5.5),
        )),
    ),
    # spi/ssd1309_spi/ssd1309_spi.c:24-28: SPI0 with its own chip select,
    # D/C on GP20, RES# on GP21; spi=(SCK, MOSI, CS, D/C). The PL022 raises
    # chip select between bytes (Motorola mode, CPHA 0).
    "ssd1309_spi": (
        Attach("ssd1309", "oled", {"spi": (18, 19, 17, 20), "reset": (21,)}, checks=(
            Check("part.read_region('gddram', 0, 1024)", lambda v: displays.ssd1309_spi_frame(v, least=2),
                  "the alphabet on rows 0 and 1 and a counter of at least 2 on row 3", 2.5),
            Check("(part.display_on, part.addressing_mode)", lambda v: v == (True, 2),
                  "display on, page addressing as after RES#", 2.5),
        )),
    ),
    # spi/ssd1309_stdout_spi/ssd1309_stdout_spi.c:39-43: the same pins; a
    # DMA refresh of the whole frame every 20 ms while it changes.
    "ssd1309_stdout_spi": (
        Attach("ssd1309", "oled", {"spi": (18, 19, 17, 20), "reset": (21,)}, checks=(
            Check("part.read_region('gddram', 0, 1024)", displays.ssd1309_stdout_frame,
                  "Hello, World on row 0 from byte 16, and the snake below it", 2.0),
            Check("(part.display_on, part.addressing_mode, part.column_range, part.page_range)",
                  lambda v: v == (True, 0, (0, 127), (0, 7)),
                  "display on, horizontal addressing over the whole frame", 2.0),
        )),
    ),
    # i2c/ht16k33_i2c/ht16k33_i2c.c:37, 90-94: 0x70. The scroll runs from
    # the start for 29 steps of 150 ms.
    "ht16k33_i2c": (
        Attach("ht16k33", "leds", {"i2c": (4, 5, 0x70)}, {"i2c": 2}, checks=(
            Check("part.read_region('display', 0, 16)", displays.ht16k33_scrolling,
                  "four consecutive characters of the scrolled string on digits 0-3", 2.0),
            Check("(part.oscillator, part.display_on, part.blink, part.row_int)",
                  lambda v: v == (True, True, 0, 0), "ht16k33_init()'s settings", 2.0),
        )),
    ),
    # i2c/lcd_1602_i2c/lcd_1602_i2c.c:62: 0x27. The first pair of lines is
    # written by about 0.2 s and stays until 2.2 s.
    "lcd_1602_i2c": (
        Attach("hd44780_pcf8574", "lcd", {"i2c": (4, 5, 0x27)}, {"i2c": 2}, checks=(
            Check("part.read_region('ddram', 0, 128)", displays.lcd_showing_a_screen,
                  "a pair of the program's messages, centred, the rest of the DDRAM spaces", 1.0),
            Check("(part.display_on, part.cursor, part.blink, part.two_lines, part.font_5x10, part.eight_bit, "
                  "part.increment, part.backlight)",
                  lambda v: v == (True, False, False, True, False, False, True, True),
                  "lcd_init()'s settings in 4-bit mode, backlight on", 1.0),
        )),
    ),
    # spi/max7219_32x8_spi/max7219_32x8_spi.c:33, 92-114: SPI0, chip select
    # on GP17 by hand, four modules; spi=(SCK, MOSI, CS). The ramp of digit
    # values takes about 5.2 s.
    "max7219_32x8_spi": (
        Attach("max7219", "matrix", {"spi": (18, 19, 17)}, properties={"chain": 4}, checks=(
            Check("part.read_region('digits', 0, 32)", lambda v: displays.max7219_ramp(v, least=20),
                  "all four modules alike on one step of the 0..255 ramp, at least 20", 2.0),
            Check("(part.decode_mode[:4], part.scan_limit[:4], part.shutdown[:4], part.intensity[:4], "
                  "part.display_test[:4])",
                  lambda v: v == ((0,) * 4, (7,) * 4, (1,) * 4, (8,) * 4, (0,) * 4),
                  "the init sequence's registers on all four modules", 2.0),
        )),
    ),
    # spi/max7219_8x7seg_spi/max7219_8x7seg_spi.c:33, 110-132: one module.
    "max7219_8x7seg_spi": (
        Attach("max7219", "digits", {"spi": (18, 19, 17)}, checks=(
            Check("part.read_region('digits', 0, 8)", lambda v: displays.max7219_counting(v, least=200),
                  "a decimal count of at least 200, digit 0 the least significant", 2.0),
            Check("(part.decode_mode[0], part.scan_limit[0], part.shutdown[0], part.intensity[0], "
                  "part.display_test[0])",
                  lambda v: v == (0xFF, 7, 1, 8, 0), "the init sequence's registers", 2.0),
        )),
    ),
    # uart/gps_uart/gps_uart.c:20-23: uart1 at 9600, the program's TX on GP4
    # and RX on GP5, so the part's TX (the binding's first pin) is GP5. The
    # program takes $GPRMC only (line 147), which a u-blox M8 sends with GPS
    # as its one GNSS (gnss=1).
    "gps_uart": (
        Attach("neo_m8n", "gps", {"uart": (5, 4)}, {},
               {"gnss": 1, "latitude": 473977420, "longitude": -1223321500, "speed": 1500, "heading": 9000000,
                "satellites": 8, "fix": 3, "time": (2026, 11, 15, 12, 34, 56)}),
    ),
    # i2c/pa1010d_i2c/pa1010d_i2c.c:26: 0x10 on i2c_default. Its PMTK 314
    # (line 125) leaves RMC alone; the part keeps what it was told.
    "pa1010d_i2c": (
        Attach("pa1010d", "gnss", {"i2c": (4, 5, 0x10)}, {"i2c": 2},
               {"latitude": 473977420, "longitude": -1223321500, "speed": 1500, "heading": 9000000,
                "satellites": 8, "fix": 3, "time": (2026, 11, 15, 12, 34, 56)},
               checks=(Check("part.nmea_rates", lambda v: tuple(v) == (0, 1, 0, 0, 0, 0),
                             "the program's PMTK 314 set RMC every fix and every other sentence off"),
                       Check("part.packets", lambda v: v >= 2, "the program read the I2C buffer"))),
    ),
    # uart/lcd_uart/lcd_uart.c:29-31: uart1 at 9600, TX only, on GP8; the
    # part's TX would reach GP9, which the program leaves alone. It prints
    # nothing. As built it sends 0xFE 0x42, Display On, and faults before
    # the argument byte: GCC turned lcd_set_backlight's read through a NULL
    # buffer (line 101) into a UDF in lcd_init (see program_parts_check.c).
    # Each run leaves a Display On waiting for its argument, which the next
    # run's 0xFE completes; no setting changes. The Pico 2's stdio_init_all
    # lets about 2.5 s pass before lcd_init.
    "lcd_uart": (
        Attach("lcd_backpack", "lcd", {"uart": (9, 8)}, {}, {},
               # Counted from just before the judged restart, the DUT halted:
               # the part is attached while an earlier run may be between its
               # two bytes, and a snapshot taken after the restart could
               # land past this run's first byte on a loaded emulator. The
               # frame errors since then show a byte the line garbled.
               checks=(Check("(part.stats('uart')['bytes_rx'] - lcd_at_restart['bytes_rx'], "
                             "part.stats('uart')['frame_errors'] - lcd_at_restart['frame_errors'])",
                             lambda v: v[0] == 2,
                             "the run sent 0xFE 0x42 and nothing after its fault", 5.0,
                             setup="lcd_at_restart = part.stats('uart')", before_restart=True),
                       Check("(part.contrast, part.brightness, part.columns, part.rows)",
                             lambda v: tuple(v) == (200, 255, 16, 2), "no setting changed from the factory state", 5.0),
                       Check("bytes(part.read_region('splash', 0, 16)) + bytes(part.read_region('splash', 20, 16))",
                             lambda v: v == b"\xff" * 32, "the splash is still the built-in one", 5.0))),
    ),
    # spi/mpu9250_spi/mpu9250_spi.c:43-46: MISO GP4, CS GP5, SCK GP6, MOSI GP7.
    "mpu9250_spi": (
        Attach("mpu9250", "imu", {"spi": (6, 7, 4, 5)}, {},
               {"acceleration": (-125, 750, 980), "angular_rate": (5000, -12000, 250), "temperature": 31000}),
    ),
    # spi/spi_flash/spi_flash.c and pio/spi/spi_flash.c: spi_default's pins,
    # chip select GP17. The programs end with the sector erased, so the part
    # counts what they did. Counted from before the judged restart, the DUT
    # halted and the part idle: the counters run from attach, across the
    # rig's runs, and a restart that lands while an earlier run's erase
    # keeps the part BUSY has the next run's first instructions ignored, as
    # a W25Q does (7.1).
    **{name: (
        Attach("w25q16jv", "flash", {"spi": (18, 19, 16, 17)}, {},
               checks=(Check(FLASH_SINCE_RESTART,
                             lambda v: v[0] >= 1 and v[1] >= 2 and tuple(v[2:]) == (0, 0),
                             "the program erased, programmed and erased the first sector, nothing refused",
                             setup=FLASH_AT_RESTART, before_restart=True),)),
    ) for name in ("spi_flash", "pio_spi_flash")},
    # Timed single-wire and clocked parts, behind the wire engines. Most of
    # these programs use GP2 and GP3, which reach no harness pin on fixture
    # 1 (they are its SWD lines), so they run on the carrier only.
    # pio/ws2812/ws2812.c:31-36, 126: WS2812_PIN 2 (the pico boards define
    # no PICO_DEFAULT_WS2812_PIN), 150 RGB pixels at 800 kHz.
    "pio_ws2812": (
        Attach("ws2812b", "strip", {"din": (2,)}, properties={"count": 150}, checks=(
            Check("part.read_region('leds', 0, 450)", lights.ws2812_shows_a_random_frame,
                  "a frame of 150 rand() pixels the 'Random data' pattern sent", 2.5),
            ws2812_frames("strip", 450, "frames of 450 bytes, none with a pulse out of the datasheet's windows"),
        )),
    ),
    # pio/ws2812/ws2812_parallel.c:19-20, 188-203, 295-298: strips on GP2
    # (RGB) and GP3 (RGBW), 64 pixels each, a frame of 256 values per strip.
    "pio_ws2812_parallel": (
        Attach("ws2812b", "strip0", {"din": (2,)}, properties={"count": 64, "channels": 3}, checks=(
            Check("part.read_region('leds', 0, 192)", lambda v: lights.ws2812_parallel_shows_a_frame(v, 0),
                  "strip 0 holding a frame the program dithered and sent", 2.5),
            ws2812_frames("strip0", 256, "frames of 256 bytes, none spoiled"),
        )),
        Attach("ws2812b", "strip1", {"din": (3,)}, properties={"count": 64, "channels": 4}, checks=(
            Check("part.read_region('leds', 0, 256)", lambda v: lights.ws2812_parallel_shows_a_frame(v, 1),
                  "strip 1 holding a frame the program dithered and sent, W always 0", 2.5),
            ws2812_frames("strip1", 256, "frames of 256 bytes, none spoiled"),
        )),
    ),
    # pio/apa102/apa102.c:14-21: CLK on GP2, DIN on GP3, 150 LEDs at 5 MHz;
    # spi=(clock, data).
    "pio_apa102": (
        Attach("apa102", "strip", {"spi": (2, 3)}, properties={"count": 150}, checks=(
            # The program updates the strip without pause, and `leds` read
            # while an update shifts in shows part of one step and part of
            # the next: `frame` holds the last whole update.
            Check("bytes(part.read_region('frame', 0, 600))", lights.apa102_shows_a_frame,
                  "one step of the three sine waves, at global brightness 16", 2.0),
            Check("(part.frames > 10, part.end_frames > 10)", lambda v: v == (True, True),
                  "start and end frames around every update", 2.0),
        )),
    ),
    # pio/onewire/onewire.c:21-22: the bus on GP15. 21.375 degC is 342
    # sixteenths, exact at the default 12 bits.
    "pio_onewire": (
        Attach("ds18b20", "thermo", {"dq": (15,)}, properties={"temperature": 21375, "serial": 0xC0FFEE},
               checks=(
                   Check("(part.searches > 0, part.matches > 1, part.conversions > 1, "
                         "part.scratchpad_reads > 1)", lambda v: v == (True, True, True, True),
                         "a search that found the part, conversions, and its scratchpad read by its ROM", 4.0),
               ), needs_line_pullups=True),
    ),
    # gpio/dht_sensor/dht.c:16: the sensor on GP15.
    "dht": (
        Attach("dht22", "sensor", {"data": (15,)}, properties={"humidity": 45600, "temperature": 23400},
               checks=(Check("part.responses > 1", lambda v: v is True,
                             "the program's start signals answered", 5.0),),
               needs_line_pullups=True),
    ),
    # pio/ir_nec/ir_loopback/ir_loopback.c:24-25: the IR LED on GP14, the
    # receiver's output on GP15; ir=(led, out).
    "pio_ir_loopback": (
        Attach("tsop38238", "receiver", {"ir": (14, 15)}, checks=(
            Check("(part.frames > 1, part.address)", lambda v: v == (True, 0),
                  "NEC frames to address 0 through the IR path", 3.5),
        )),
    ),
    # pio/hub75/hub75.c:16-22: R0 G0 B0 R1 G1 B1 on GP0-5, A-E on GP6-10,
    # CLK GP11, STROBE (latch) GP12, OEn GP13; clock=(clk, latch, oe).
    "pio_hub75": (
        Attach("hub75", "panel", {"clock": (11, 12, 13), "upper": (0, 1, 2), "lower": (3, 4, 5),
                                  "rows": (6, 7, 8, 9), "e": (10,)}, checks=(
            Check("part.slots", lambda v: v == 256, "all 32 rows seen with their 8 enable times", 8.0),
            Check("part.read_region('thumbnail', 0, 3072)", lights.hub75_shows_the_mountains,
                  "every eighth column of the mountains, gamma-corrected, within 2 counts", 8.0),
        )),
    ),
}
