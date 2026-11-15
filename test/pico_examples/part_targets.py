# SPDX-License-Identifier: Apache-2.0 OR MIT
# Console criteria for the pico-examples programs that need an external
# part, kept beside the parts they attach (parts.py) as each model lands.
# Same rules as targets.py: every pattern comes from the program's source,
# and the numbers from its own formatting of the properties parts.py sets
# (test/unit/test_part_models.py runs the models' arithmetic and the
# programs' conversions to check them).

from test.pico_examples.targets import FORBIDDEN, NL, Spec, forbid


def _twice(*lines: str) -> tuple[str, ...]:
    return lines + lines


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
}
