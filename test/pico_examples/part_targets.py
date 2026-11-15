# SPDX-License-Identifier: Apache-2.0 OR MIT
# Console criteria for the pico-examples programs that need an external
# part, kept beside the parts they attach (parts.py) as each model lands.
# Same rules as targets.py: every pattern comes from the program's source,
# and the numbers from its own formatting of the properties parts.py sets
# (test/unit/test_part_models.py runs the models' arithmetic and the
# programs' conversions to check them).

from test.pico_examples.targets import FORBIDDEN, NL, Spec, _rows, forbid


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
}


def _gpio_wakes(asleep: str) -> tuple[str, ...]:
    # The first run starts wherever the restart caught GP15; every later
    # one starts at a rising edge, so GP15 is high and the program waits
    # for the next dip before it sleeps.
    return (r"State of gpio 15 is [01]" + NL,
            r"Wake up, test run: 1" + NL + r"(?:Awake until gpio 15 goes low" + NL + r")?" + asleep + NL,
            *(rf"Wake up, test run: {n}" + NL + r"Awake until gpio 15 goes low" + NL + asleep + NL
              for n in (2, 3, 4)))


def _bar(low: int, high: int) -> str:
    # One of joystick_display's 40-wide bars with its 'o' at a position in
    # [low, high].
    return rf"\[(?= {{{low},{high}}}o)[ o]{{40}}\]"


PART_SPECS.update({
    # low_power/low_power_dormant/low_power_dormant_gpio.c:31-58 and
    # low_power/low_power_sleep/low_power_sleep_gpio.c:23-49, with GP15 from
    # parts.py: one run a second.
    "low_power_dormant_gpio": Spec(
        console=_gpio_wakes(r"Dormant until gpio 15 goes high"),
        seconds=8.0,
        forbidden=forbid(r"returned error"),
        source="low_power/low_power_dormant/low_power_dormant_gpio.c",
    ),
    "low_power_sleep_gpio": Spec(
        console=_gpio_wakes(r"Sleeping until gpio 15 goes high"),
        seconds=8.0,
        forbidden=forbid(r"returned error"),
        source="low_power/low_power_sleep/low_power_sleep_gpio.c",
    ),
    # low_power/low_power_pstate/low_power_pstate_gpio.c:21-48 (pico2): every
    # wake is a boot; the run count persists across them but starts at
    # whatever the RAM held. Reaching the printf after the power-down is the
    # failure.
    "low_power_pstate_gpio": Spec(
        console=(r"Wake up, test run: \d+" + NL + r"State of gpio 15 is [01]" + NL,
                 r"Low power until gpio 15 goes high" + NL,
                 *(r"Wake up, test run: \d+" + NL + r"State of gpio 15 is 1" + NL + r"Awake until gpio 15 goes low"
                   + NL + r"Low power until gpio 15 goes high" + NL,) * 2),
        seconds=8.0,
        forbidden=forbid(r"returned error"),
        source="low_power/low_power_pstate/low_power_pstate_gpio.c",
    ),
    # gpio/hello_gpio_irq/hello_gpio_irq.c:16-31: one line per edge on GP2,
    # each a single event 200 ms from the last.
    "hello_gpio_irq": Spec(
        console=(r"Hello GPIO IRQ" + NL, *(r"GPIO 2 EDGE_RISE" + NL + r"GPIO 2 EDGE_FALL" + NL,) * 3),
        seconds=3.0,
        forbidden=forbid(r"LEVEL_", r", EDGE_"),
        source="gpio/hello_gpio_irq/hello_gpio_irq.c",
    ),
    # gpio/hello_7segment/hello_7segment.c:48; the count is judged by its
    # pins (pin_specs.py).
    "hello_7segment": Spec(
        console=(r"Hello, 7segment - press button to count down!" + NL,),
        seconds=3.0,
        source="gpio/hello_7segment/hello_7segment.c",
    ),
    # adc/joystick_display/joystick_display.c:18-39: every 50 ms "\rX: [",
    # 40 characters with an 'o' at raw * 40 / 4095, "]  Y: [", the same,
    # "]". At 25 % and 75 % of 3.3 V, raw is about 1024 and 3071: positions
    # 10 and 29, give or take the harness's and the DUT's 3V3. A pin held low
    # puts X at 0; held high, Y reads 4095 or a count under it, position 40
    # (no 'o') or 39.
    "joystick_display": Spec(
        console=(r"\rX: " + _bar(9, 11) + r"  Y: " + _bar(28, 31),) * 3,
        seconds=3.0,
        source="adc/joystick_display/joystick_display.c",
        analog_pins=(26, 27),
        without_analog={"console": (r"\rX: \[o {39}\]  Y: \[ {39}[ o]\]",) * 3},
    ),
    # adc/microphone_adc/microphone_adc.c:32-46: the banner, then GP26 as
    # raw * 3.3 / 4095 every 10 ms: 1.32 V at 40 % of 3.3 V, within a few
    # per cent; 0.00 V, or a count or two above, when held low.
    "microphone_adc": Spec(
        console=(r"Beep boop, listening\.\.\." + NL, *(r"1\.(?:2[5-9]|3\d)" + NL,) * 5),
        seconds=3.0,
        source="adc/microphone_adc/microphone_adc.c",
        analog_pins=(26,),
        without_analog={"console": (r"Beep boop, listening\.\.\." + NL, *(r"0\.0\d" + NL,) * 5)},
    ),
    # pio/uart_rx/uart_rx.c:40-73: core 1 sends the line once on uart1 and
    # the PIO receiver echoes it to the console.
    "pio_uart_rx": Spec(
        console=(r"Starting PIO UART RX example" + NL,
                 r"Hello, world from PIO! \(Plus 2 UARTs and 2 cores, for complex reasons\)" + NL),
        seconds=3.0,
        source="pio/uart_rx/uart_rx.c",
    ),
    # pio/uart_rx/uart_rx_intr.c:51-58, 93-162: ten lines 1-2 s apart, each
    # echoed from the PIO interrupt, then "Test complete" and main returns.
    "pio_uart_rx_intr": Spec(
        console=(r"Starting PIO UART RX interrupt example" + NL,
                 *(rf"Hello, world from PIO with interrupts! {n}" + NL for n in range(10)), r"Test complete" + NL),
        seconds=25.0,
        source="pio/uart_rx/uart_rx_intr.c",
    ),
    # pio/uart_dma/uart_dma.c:75-90, 104-241: the sentence out of the PIO
    # transmitter by DMA at 921600 baud and into the receiver by DMA; each
    # channel's interrupt says it is done, then the comparison.
    "pio_uart_dma": Spec(
        console=(r"dma_tx done" + NL, r"dma_rx done" + NL, r"Test passed" + NL),
        seconds=3.0,
        forbidden=forbid(r"Test failed"),
        source="pio/uart_dma/uart_dma.c",
    ),
    # pio/manchester_encoding/manchester_encoding.c:17-43 and
    # pio/differential_manchester/differential_manchester.c:16-43: three
    # words sent on GP2 and decoded on GP3.
    "pio_manchester_encoding": Spec(
        console=(r"Transmit program loaded at \d+" + NL, r"Receive program loaded at \d+" + NL,
                 r"00000000" + NL, r"0ff0a55a" + NL, r"12345678" + NL),
        seconds=3.0,
        source="pio/manchester_encoding/manchester_encoding.c",
    ),
    "pio_differential_manchester": Spec(
        console=(r"Transmit program loaded at \d+" + NL, r"Receive program loaded at \d+" + NL,
                 r"00000000" + NL, r"0ff0a55a" + NL, r"12345678" + NL),
        seconds=3.0,
        source="pio/differential_manchester/differential_manchester.c",
    ),
    # pwm/measure_duty_cycle/measure_duty_cycle.c:19-72: each duty on GP2
    # measured on GP5 over sleep_ms(10). The level is duty x 1001 truncated
    # (49.95 % for 50 %), and the window runs a little past 10 ms.
    "pwm_measure_duty_cycle": Spec(
        console=(r"PWM duty cycle measurement example" + NL,
                 r"Output duty cycle = 0\.0%, measured input duty cycle = 0\.0%" + NL,
                 r"Output duty cycle = 10\.0%, measured input duty cycle = (?:9\.[89]|10\.[01])%" + NL,
                 r"Output duty cycle = 50\.0%, measured input duty cycle = (?:49\.[89]|50\.[01])%" + NL,
                 r"Output duty cycle = 90\.0%, measured input duty cycle = (?:89\.[89]|90\.[01])%" + NL,
                 r"Output duty cycle = 100\.0%, measured input duty cycle = (?:99\.\d|100\.[0-2])%" + NL),
        seconds=3.0,
        source="pwm/measure_duty_cycle/measure_duty_cycle.c",
    ),
    # pio/clocked_input/clocked_input.c:29-75: eight rand() bytes clocked
    # out of SPI0 at clk_sys / 6 and into the PIO, each compared.
    "pio_clocked_input": Spec(
        console=(r"SPI running at \d+ Hz" + NL, r"Data to transmit:" + NL, r"(?:[0-9a-f]{2}" + NL + r"){8}",
                 r"Reading back from RX FIFO:" + NL, r"(?:[0-9a-f]{2} OK" + NL + r"){8}", r"Done\." + NL),
        seconds=3.0,
        forbidden=forbid(r" FAIL"),
        source="pio/clocked_input/clocked_input.c",
    ),
    # i2c/slave_mem_i2c/slave_mem_i2c.c:90-126: every 2 s the message is
    # written at 0x00, 0x20, 0x40, ..., read back in two parts (5 bytes,
    # then the rest from mem_address + 5); a mismatch asserts.
    "slave_mem_i2c": Spec(
        console=(r"I2C slave example" + NL,
                 *(rf"Write at 0x{a:02X}: 'Hello, I2C slave! - 0x{a:02X}'" + NL + rf"Read  at 0x{a:02X}: 'Hello'" + NL
                   + rf"Read  at 0x{a + 5:02X}: ', I2C slave! - 0x{a:02X}'" + NL for a in (0x00, 0x20, 0x40))),
        seconds=8.0,
        forbidden=forbid(r"Couldn't write to slave"),
        source="i2c/slave_mem_i2c/slave_mem_i2c.c",
    ),
    # i2c/slave_mem_i2c/slave_mem_i2c_burst.c:92-136: the same a byte at a
    # time, 1 ms apart, read back whole.
    "slave_mem_i2c_burst": Spec(
        console=(r"I2C slave example" + NL,
                 *(rf"Write at 0x{a:02X}: 'Hello, I2C slave! - 0x{a:02X}'" + NL
                   + rf"Read  at 0x{a:02X}: 'Hello, I2C slave! - 0x{a:02X}'" + NL for a in (0x00, 0x20, 0x40))),
        seconds=8.0,
        forbidden=forbid(r"Couldn't write to slave"),
        source="i2c/slave_mem_i2c/slave_mem_i2c_burst.c",
    ),
})

_SLAVE_BUFFER = (r"SPI slave says: When reading from MOSI, the following buffer will be written to MISO:" + NL
                 + "".join(_rows(lambda i: 255 - i)))

PART_SPECS.update({
    # spi/spi_master_slave/spi_slave/spi_slave.c:58-90: the banner, the ff..00
    # buffer, then for every page the rig's controller clocks (parts.py: the
    # master's 00..ff, 500 ms apart once told) the page it read.
    "spi_slave": Spec(
        console=(r"SPI slave example" + NL, _SLAVE_BUFFER,
                 *(rf"SPI slave says: read page {p} from the MOSI line:" + NL + "".join(_rows(lambda i: i))
                   for p in range(3))),
        seconds=6.0,
        source="spi/spi_master_slave/spi_slave/spi_slave.c",
        input=((_SLAVE_BUFFER, "peer.pages = 3"),),
    ),
})
