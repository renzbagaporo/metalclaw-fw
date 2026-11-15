# SPDX-License-Identifier: Apache-2.0 OR MIT
# Console criteria for the pico-examples programs that need an external
# part, kept beside the parts they attach (parts.py) as each model lands.
# Same rules as targets.py: every pattern comes from the program's source.

from test.pico_examples.targets import Spec

PART_SPECS: dict[str, Spec] = {}


# ---- Batch B: the INA2xx, MPU-6050, LIS3DH, MMA8451Q and MPL3115A2
# programs, with the properties parts.py sets. Each number is the program's
# arithmetic applied to the register words the model derives from those
# properties; test/unit/test_part_models.py runs both on the host and
# matches these patterns against what comes out.
from test.pico_examples.targets import NL, forbid  # noqa: E402


def _twice(*lines: str) -> tuple[str, ...]:
    return lines + lines


PART_SPECS.update({
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
})
# ---- end of batch B
