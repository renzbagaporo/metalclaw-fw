# SPDX-License-Identifier: Apache-2.0 OR MIT
# What the pico-examples programs the model checks transcribe (pico-examples
# 0d62f75) print, given the properties parts.py sets: every pattern from the
# program's source, and the numbers from its own formatting of those
# properties. The checks (test_part_models.py, test_program_parts.py,
# test_wire_models.py, test_display_models.py) run a model behind the
# program's transcription and match what comes out against these.

import re

from test.unit.programs import lights
from test.zephyr_samples.spec import FORBIDDEN, NL, Spec, forbid


def _rows(byte) -> tuple[str, ...]:
    # printbuf of a 256-byte page whose byte i is byte(i), in spi_master_slave
    # and both spi_flash programs: sixteen "%02x" a row, a space between, a
    # newline after the last.
    return tuple(" ".join(f"{byte(16 * r + c):02x}" for c in range(16)) + NL for r in range(16))


def _twice(*lines: str) -> tuple[str, ...]:
    return lines + lines


# printbuf() in spi/spi_flash/spi_flash.c:97-104 and pio/spi/spi_flash.c:94-97
# prints a page as spi_master_slave's does (targets._rows).


def _flash(*banner: str) -> tuple[str, ...]:
    erased = _rows(lambda i: 0xFF)
    return (*banner, r"After erase:" + NL, *erased, r"After program:" + NL, *_rows(lambda i: i),
            r"Erase again:" + NL, *erased)


PART_SPECS: dict[str, Spec] = {
    # bmp280_i2c.c:223 prints the greeting (its spelling), then from 243 on
    # every 500 ms printf("Pressure = %.3f kPa\n", pressure / 1000.f) and
    # printf("Temp. = %.2f C\n", temperature / 100.f): 98765 Pa, 23.45 degC.
    "bmp280_i2c": Spec(
        console=(r"Hello, BMP280! Reading temperaure and pressure values from sensor\.\.\.\r?\n",)
        + (r"Pressure = 98\.765 kPa\r?\n", r"Temp\. = 23\.45 C\r?\n") * 3,
        seconds=5.0,
        source="i2c/bmp280_i2c/bmp280_i2c.c",
    ),
    # mcp9808_i2c.c:110 greets, 121 writes the limits (30, 20, 40 degC), then
    # every second mcp9808_check_limits() prints a line per set flag and 142
    # printf("Ambient temperature: %.4f°C\n"): 34.562 degC is 553/16 =
    # 34.5625, above TUPPER only.
    "mcp9808_i2c": Spec(
        console=(r"Hello, MCP9808! Reading raw data from registers\.\.\.\r?\n",)
        + (r"Temperature is above the upper temperature limit\.\r?\n",
           r"Ambient temperature: 34\.5625\u00b0C\r?\n") * 3,
        seconds=5.0,
        forbidden=FORBIDDEN + (r"below the lower temperature limit", r"above the critical temperature limit"),
        source="i2c/mcp9808_i2c/mcp9808_i2c.c",
    ),
    # pcf8523_i2c.c:126 greets; 137..141 reset the part, write 0 to every
    # time register, set the minute alarm to 01 and check it once. From 147
    # every 500 ms it prints the time (line 152), the date (153, weekday 0
    # is Sunday) and whether AF is set (154, no newline). The first reading
    # is in the first second; the minute alarm rings at 00:01:00.
    "pcf8523_i2c": Spec(
        console=(
            r"Hello, PCF8520! Reading raw data from registers\.\.\.\r?\n",
            r"Alarm not triggered yet",
            r"Time: 00 : 00 : 0[01]\r?\nDate: Sunday 00 / 00 / 00\r?\nAlarm not triggered yet",
            r"Time: 00 : 00 : 05\r?\n",
            r"Time: 00 : 00 : 59\r?\nDate: Sunday 00 / 00 / 00\r?\nAlarm not triggered yet",
            r"Time: 00 : 01 : 00\r?\nDate: Sunday 00 / 00 / 00\r?\nALARM RINGING",
        ),
        seconds=70.0,
        source="i2c/pcf8523_i2c/pcf8523_i2c.c",
    ),
    # bme280_spi.c:194 greets, 214 printf("Chip ID is 0x%x\n", id), then
    # every second (232..234) humidity / 1024.0 as %.2f%%, the pressure as
    # %dPa and temperature / 100.0 as %.2fC: 45.67 %RH, 99887 Pa, 21.37 degC.
    "bme280_spi": Spec(
        console=(r"Hello, bme280! Reading raw data from registers via SPI\.\.\.\r?\n",
                 r"Chip ID is 0x60\r?\n")
        + (r"Humidity = 45\.67%\r?\n", r"Pressure = 99887Pa\r?\n", r"Temp\. = 21\.37C\r?\n") * 3,
        seconds=6.0,
        source="spi/bme280_spi/bme280_spi.c",
    ),
    # The INA2xx, MPU-6050, LIS3DH, MMA8451Q and MPL3115A2 programs: each
    # number is the program's arithmetic applied to the register words the
    # model derives from the properties parts.py sets.
    # i2c/ina219_i2c/ina219_i2c.c:59, 76-102: the banner, then one line a
    # second. 123.4 mA through 0.1 ohm is 12.34 mV, 1234 at 10 uV; 5124 mV is
    # 1281 at 4 mV; CURRENT 1234 x 4096 / 4096 = 1234 at 0.1 mA; POWER
    # 1234 x 1281 / 5000 = 316 at 2 mW; VIN+ = 5.124 + 0.01234 V.
    "ina219_i2c": Spec(
        console=(r"INA219 example" + NL, *_twice(
            r"VIN\+: 5\.136 V  VIN-: 5\.124 V  shunt: 12\.340 mV  current: 123\.40 mA  power: 632\.00 mW" + NL)),
        forbidden=forbid(r"Math overflow"),
        source="i2c/ina219_i2c/ina219_i2c.c",
    ),
    # i2c/ina228_i2c/ina228_i2c.c:285-308, stock build: no banner, one line
    # a second. SHUNT_CAL (line 153) is 75; 92.5 mA through 15 mohm is
    # 1.3875 mV, VSHUNT 4440 at 312.5 nV; CURRENT 4440 x 4096 / 75 = 242483
    # at 0.2 / 2^19 A; VBUS 4850 mV is 24832 at 195.3125 uV; POWER
    # 242483 x 24832 / 16384 = 367512 at 3.2 x CURRENT_LSB W.
    "ina228_i2c": Spec(
        console=_twice(r"current: 92\.50 mA  voltage: 4\.850 V  power: 448\.62 mW" + NL),
        forbidden=forbid(r"current: invalid"),
        source="i2c/ina228_i2c/ina228_i2c.c",
    ),
    # i2c/ina237_i2c/ina237_i2c.c:84, 106-139: the banner, then a triggered
    # conversion and one line a second. ADC range 1 and SHUNT_CAL 375 (line
    # 30); 200 mA through 15 mohm is 3 mV, VSHUNT 2400 at 1.25 uV; CURRENT
    # 2400 x 4096 / 375 = 26214 at 0.25 / 32768 A; VBUS 3300 mV is 1056 at
    # 3.125 mV; POWER 26214 x 1056 / 64 = 432531 at 0.2 x CURRENT_LSB W;
    # DIETEMP 31.25 C is 250 at 125 m°C, in bits 15:4.
    "ina237_i2c": Spec(
        console=(r"INA237 example" + NL, *_twice(
            r"bus: 3\.300 V  shunt: 3\.000 mV  current: 200\.00 mA  power: 659\.99 mW \(calc: 659\.99 mW\)  "
            r"temp: 31\.25 C" + NL)),
        source="i2c/ina237_i2c/ina237_i2c.c",
    ),
    # i2c/ina260_i2c/ina260_i2c.c:93, 109-127: the banner, then one line a
    # second. 742.5 mA is 594 at 1.25 mA; 5000 mV is 4000 at 1.25 mV; POWER
    # 594 x 4000 / 6400 = 371 at 10 mW.
    "ina260_i2c": Spec(
        console=(r"ina260 example" + NL, *_twice(r"current: 742\.50 mA voltage: 5\.00 V power: 3710 mW" + NL)),
        source="i2c/ina260_i2c/ina260_i2c.c",
    ),
    # i2c/mpu6050_i2c/mpu6050_i2c.c:93, 104-119: the banner, the reset, then
    # raw counts every 100 ms. At +-2 g, 16384 LSB/g: 250, -500, 1125 mg are
    # 4096, -8192, 18432; at +-250 deg/s, 131 LSB/(deg/s): 10, -2, 3 deg/s
    # are 1310, -262, 393; 26.53 C is (26.53 - 36.53) x 340 = -3400, which
    # the program turns back into -3400 / 340.0 + 36.53.
    "mpu6050_i2c": Spec(
        console=(r"Hello, MPU6050! Reading raw data from registers\.\.\." + NL, *_twice(
            r"Acc\. X = 4096, Y = -8192, Z = 18432" + NL, r"Gyro\. X = 1310, Y = -262, Z = 393" + NL,
            r"Temp\. = 26\.530000" + NL)),
        source="i2c/mpu6050_i2c/mpu6050_i2c.c",
    ),
    # i2c/lis3dh_i2c/lis3dh_i2c.c:93, 106-126: the banner, then a block
    # every 500 ms. Normal mode at +-2 g (CTRL_REG1 0x97, CTRL_REG4 0x80) is
    # 4 mg per digit, left-justified by 6 bits, and the program divides by
    # 64 / 0.004: 252, -500, 1020 mg read 0.252, -0.500, 1.020 g. The
    # temperature is 4 digits per C from 25 C, so 28.75 C is 15 digits,
    # which the program prints as raw / 64. The degree sign is byte 176,
    # one replacement character once decoded.
    "lis3dh_i2c": Spec(
        console=(r"Hello, LIS3DH! Reading raw data from registers\.\.\." + NL, *_twice(
            r"TEMPERATURE: 15\.000.C" + NL, r"ACCELERATION VALUES: " + NL, r"X acceleration: 0\.252g" + NL,
            r"Y acceleration: -0\.500g" + NL, r"Z acceleration: 1\.020g" + NL)),
        source="i2c/lis3dh_i2c/lis3dh_i2c.c",
    ),
    # i2c/mma8451_i2c/mma8451_i2c.c:74, 89-125: the banner, then a block
    # every 500 ms. +-4 g (range_config 1, line 40) is 2048 counts/g: 500,
    # -250, 2000 mg are 1024, -512, 4096, times 9.81f / 2048.
    "mma8451_i2c": Spec(
        console=(r"Hello, MMA8451! Reading raw data from registers\.\.\." + NL, *_twice(
            r"ACCELERATION VALUES: " + NL, r"X acceleration: 4\.905000ms\^-2" + NL,
            r"Y acceleration: -2\.452500ms\^-2" + NL, r"Z acceleration: 19\.620001ms\^-2" + NL)),
        source="i2c/mma8451_i2c/mma8451_i2c.c",
    ),
    # i2c/mpl3115a2_i2c/mpl3115a2_i2c.c:190, 207-230: the banner, then a
    # line each time the FIFO overflows INT1. Altimeter mode at OS 128 and
    # ST 0 takes a sample a second, so 32 fill it after about 32 s. 321.5 m
    # is 5144 in Q16.4 and 21.625 C is 346 in Q8.4; every sample is the same,
    # so the averages are those values.
    "mpl3115a2_i2c": Spec(
        console=(r"Hello, MPL3115A2\. Waiting for something to interrupt me!\.\.\." + NL,
                 r"32 sample average -> t: 21\.6250 C, h: 321\.5000 m" + NL),
        seconds=45.0,
        source="i2c/mpl3115a2_i2c/mpl3115a2_i2c.c",
    ),
    # The display programs print a banner, if anything; what they draw is
    # judged by parts.py's checks. ssd1309_spi and lcd_1602_i2c print
    # nothing (lcd_1602_i2c.c never calls stdio_init_all).
    # ssd1306_i2c.c:319.
    "ssd1306_i2c": Spec(
        console=(r"Hello, SSD1306 OLED display! Look at my raspberries\.\." + NL,),
        seconds=2.0,
        source="i2c/ssd1306_i2c/ssd1306_i2c.c",
    ),
    # ssd1309_stdout_spi.c:209: the printf reaches the UART driver
    # stdio_init_all() enabled as well as the frame buffer's.
    "ssd1309_stdout_spi": Spec(
        console=(r"Hello, World" + NL,),
        seconds=2.0,
        source="spi/ssd1309_stdout_spi/ssd1309_stdout_spi.c",
    ),
    # ht16k33_i2c.c:164, its spelling.
    "ht16k33_i2c": Spec(
        console=(r"Welcome to HT33k16!" + NL,),
        seconds=2.0,
        source="i2c/ht16k33_i2c/ht16k33_i2c.c",
    ),
    # max7219_32x8_spi.c:89.
    "max7219_32x8_spi": Spec(
        console=(r"Hello, max7219! Drawing things on a LED matrix since 2022\.\.\." + NL,),
        seconds=2.0,
        source="spi/max7219_32x8_spi/max7219_32x8_spi.c",
    ),
    # max7219_8x7seg_spi.c:107.
    "max7219_8x7seg_spi": Spec(
        console=(r"Hello, max7219! Drawing things on a 8 x 7 segment display since 2022\.\.\." + NL,),
        seconds=2.0,
        source="spi/max7219_8x7seg_spi/max7219_8x7seg_spi.c",
    ),
    # uart/gps_uart/gps_uart.c:246 greets; every line it reads it echoes
    # (line 74, the sentence's own CR LF included); a $GPRMC with status A
    # (147-184) prints the coordinates as floats (253-254). 47.3977420 N is
    # 4723.86452 in the M8's ddmm.mmmmm, which the program's float
    # arithmetic (118-125) turns into 47.397739; 122.3321500 W likewise into
    # -122.332146. A fix a second; the first line may be cut short.
    "gps_uart": Spec(
        console=(r"Waiting for NMEA data" + NL, *(
            r"Full NMEA String: \$GPRMC,\d{6}\.\d{2},A,4723\.86452,N,12219\.92900,W,[^\r\n]*\r*\n",
            r"Valid GPS Data Received" + NL, r"Latitude: 47\.397739" + NL, r"Longitude: -122\.332146" + NL) * 3),
        seconds=6.0,
        source="uart/gps_uart/gps_uart.c",
    ),
    # i2c/pa1010d_i2c/pa1010d_i2c.c:137 greets, writes its PMTK 314 and then
    # each second reads 250 bytes, splits the $GNRMC in them at commas and
    # prints the fields (75-90). The first read comes before the part has
    # taken the command and holds the start of a full epoch, with no RMC in
    # its first 254 bytes; the program's strstr then finds none and it
    # prints from address 0 (43-44), so only later blocks are judged. The
    # PA1010D's RMC (datasheet Table-10) has hhmmss.sss, ddmm.mmmm: 47.397742
    # N is 4723.8645; 1.5 m/s is 2.92 kn; 90.00 degrees; 15 Nov 2026; no
    # magnetic variation; mode A.
    "pa1010d_i2c": Spec(
        console=(r"Hello, PA1010D! Reading raw data from module\.\.\." + NL, *_twice(
            r"Protocol:GNRMC" + NL, r"UTC Time: \d{6}\.000" + NL, r"Status: Data Valid" + NL,
            r"Latitude: 4723\.8645" + NL, r"N/S indicator: N" + NL, r"Longitude: 12219\.9290" + NL,
            r"E/W indicator: W" + NL, r"Speed over ground: 2\.92" + NL, r"Course over ground: 90\.00" + NL,
            r"Date: 15/11/26" + NL, r"Magnetic Variation: " + NL, r"E/W degree indicator: " + NL,
            r"Mode: A" + NL, r"Checksum: [0-9A-F]{2}" + NL)),
        seconds=8.0,
        source="i2c/pa1010d_i2c/pa1010d_i2c.c",
    ),
    # uart/lcd_uart/lcd_uart.c prints nothing, and faults in lcd_init as
    # built (parts.py); its parts checks judge it, and its console case
    # skips, an empty read showing nothing.
    "lcd_uart": Spec(
        console=(),
        seconds=2.0,
        source="uart/lcd_uart/lcd_uart.c",
    ),
    # spi/mpu9250_spi/mpu9250_spi.c:114 greets, 136 prints WHO_AM_I (0x71),
    # then every 100 ms raw counts (145-149). +-2 g at 16384 LSB/g: -125,
    # 750, 980 mg are -2048, 12288, 16056; +-250 deg/s at 131 LSB/(deg/s):
    # 5, -12, 0.25 deg/s are 655, -1572, 33; 31 C is (31 - 21) x 333.87 =
    # 3339 (PS-MPU-9250A-01 3.4.2), which the program reads with the
    # MPU-6050's formula: 3339 / 340.0 + 36.53.
    "mpu9250_spi": Spec(
        console=(r"Hello, MPU9250! Reading raw data from registers via SPI\.\.\." + NL,
                 r"I2C address is 0x71" + NL, *_twice(
                     r"Acc\. X = -2048, Y = 12288, Z = 16056" + NL, r"Gyro\. X = 655, Y = -1572, Z = 33" + NL,
                     r"Temp\. = 46\.350588" + NL)),
        source="spi/mpu9250_spi/mpu9250_spi.c",
    ),
    # spi/spi_flash/spi_flash.c:114 and 131 greet, then the first sector is
    # erased, read, programmed with 0..255, read, erased and read (137-155).
    "spi_flash": Spec(
        console=_flash(r"SPI flash example" + NL, r"SPI initialised, let's goooooo" + NL),
        seconds=3.0,
        source="spi/spi_flash/spi_flash.c",
    ),
    # pio/spi/spi_flash.c:106 and 119 greet; the same traffic through PIO.
    "pio_spi_flash": Spec(
        console=_flash(r"PIO SPI Example" + NL, r"Loaded program at \d+" + NL),
        seconds=3.0,
        source="pio/spi/spi_flash.c",
    ),
    # The LED, panel, 1-Wire, DHT, encoder and IR programs. pio_apa102 and
    # pio_hub75 print nothing; their parts' checks judge them.
    # pio/ws2812/ws2812.c:113, 130-133: the banner, then the first
    # pattern's name and direction, from newlib's first two rand() values.
    "pio_ws2812": Spec(
        console=(r"WS2812 Smoke Test, using pin 2" + NL, *(p + NL for p in map(re.escape,
                                                                         lights.first_pattern(False)))),
        seconds=2.0,
        source="pio/ws2812/ws2812.c",
    ),
    # ws2812_parallel.c:286, 304-308, with a third rand() for "(still)".
    "pio_ws2812_parallel": Spec(
        console=(r"WS2812 parallel using pin 2" + NL, *(p + NL for p in map(re.escape,
                                                                      lights.first_pattern(True)))),
        seconds=2.0,
        source="pio/ws2812/ws2812_parallel.c",
    ),
    # pio/onewire/onewire.c:36-66: a search finds the one part and prints
    # its ROM code, then each reading as temp / 16.0 with %f. The rom is
    # family 28h, serial C0FFEEh and their CRC; 21.375 degC is exact.
    "pio_onewire": Spec(
        console=(r"Found 1 devices" + NL, rf"\t0: 0x{lights.ds18b20_rom(0xC0FFEE):x}" + NL,
                 *(r"\t0: 21\.375000" + NL,) * 3),
        seconds=6.0,
        source="pio/onewire/onewire.c",
    ),
    # gpio/dht_sensor/dht.c:36-40: a reading every 2 s and more. 45.6 %RH and
    # 23.4 degC are 456 and 234 tenths; 23.4 * 9 / 5 + 32 is 74.12 in float.
    # The program calls a bit 1 when its high lasted more than 16 passes of
    # a sleep_us(1) loop (:57-71). On the carrier's Pico, at 125 MHz, a pass
    # takes about 1.1 us (72 passes over the AM2302's 80 us high), so a 0
    # (26-28 us, datasheet section 6) counts about 24: the decoded bytes
    # read ff ff ff ff ff, whose sum fails the check (:77), and every
    # reading is "Bad data" (:89-91). That is also all a run with no sensor
    # prints (no transitions, j < 40), and the reading printed after it is
    # an uninitialised struct (:34-38), so nothing on the pico build's
    # console shows the program read the part; a 0 short enough to read as
    # one would be out of the datasheet. Its start signals are judged
    # (parts.py).
    "dht": Spec(
        boards=("pico2",),
        other_boards=("at 125 MHz a pass of its sleep_us(1) loop is about 1.1 us, so a datasheet 0 bit counts over "
                      "16 and reads as 1, and every reading is the 'Bad data' a run without a sensor prints too"),
        console=(r"Humidity = 45\.6%, Temperature = 23\.4C \(74\.1F\)" + NL,) * 2,
        seconds=6.0,
        forbidden=forbid(r"Bad data"),
        source="gpio/dht_sensor/dht.c",
    ),
    # pio/ir_nec/ir_loopback/ir_loopback.c:37-59: a frame a second to
    # address 0, data counting up from 0, each received back 100 ms later.
    "pio_ir_loopback": Spec(
        console=tuple(rf"sent: 00, {d:02x}\treceived: 00, {d:02x}" for d in range(3)),
        seconds=5.0,
        forbidden=forbid(r"could not configure PIO"),
        source="pio/ir_nec/ir_loopback/ir_loopback.c",
    ),
}


