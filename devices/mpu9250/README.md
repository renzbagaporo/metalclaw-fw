<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# mpu9250

An InvenSense MPU-9250 accelerometer and gyroscope on SPI or I2C. The host
sets acceleration in milli-g, angular rate in milli-degrees per second and
the die temperature in millidegrees, and the model turns them into the
16-bit big-endian counts at the full scales the DUT selects in
`ACCEL_CONFIG` and `GYRO_CONFIG`. The AK8963 magnetometer in the same
package answers behind the MPU-9250's auxiliary I2C master and is not
modelled.

The model fills one `struct mclw_regmap`, bank 0, and arms the I2C target
engine or the SPI target engine with it. On SPI the first byte is
`R/W A6..A0`, bit 7 set to read, and a burst runs on through consecutive
registers with no dummy byte (PS-MPU-9250A-01 7.5). Its `on_write` hook
handles `PWR_MGMT_1` (reset, sleep) and the two full-scale registers.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | one of the two | an I2C target, from the tuple `(sda, scl, address)` |
| 1 | `spi` | `spi_target` | one of the two | an SPI target, from the tuple `(sck, mosi, miso, cs)` |

The I2C address is 0x68 with AD0 low, 0x69 with it high (PS 3.4.2). SPI is
modes 0 and 3, MSB first: data latched on the rising edge, changed on the
falling one (PS 7.5). The part takes 1 MHz for every register and 20 MHz for
the sensor and interrupt registers (PS 3.6, 3.6.1); the harness's ceiling for a
register map is 10.7 MHz.

```python
import metalclaw
imu = metalclaw.attach("mpu9250", "imu", spi=(6, 7, 4, 5))   # sck, mosi, miso, cs
imu.acceleration = (-125, 750, 980)        # mg
imu.angular_rate = (5000, -12000, 250)     # mdps
imu.temperature = 31000                    # mC
imu.spi_mode, imu.stats("spi")
imu.reset()
imu.detach()
```

## Properties

| name | type | unit | range | meaning |
| --- | --- | --- | --- | --- |
| `acceleration` | i32[3] | mg | -16 000 .. 16 000 | X, Y, Z; saturates at the selected range |
| `angular_rate` | i32[3] | mdps | -2 000 000 .. 2 000 000 | X, Y, Z; saturates at the selected range |
| `temperature` | i32 | mC | -40 000 .. 85 000 | die temperature |
| `spi_mode` | u32 | | 0 or 3 | the SPI mode the target listens in |

Defaults: (0, 0, 1000) mg, (0, 0, 0) mdps, 25 000 mC. Counts are the nearest
LSB: 16384 LSB/g at +-2 g, halving per `ACCEL_FS_SEL` step (PS table 2);
131 LSB/(deg/s) at +-250 deg/s, halving per `GYRO_FS_SEL` step (PS table 1);
temperature `(T - 21 C) x 333.87` (PS 3.4.2, RM 4.23: 333.87 LSB/C, 0 LSB at
21 C). There are no regions and no faults. `reset()` restores the defaults
and the power-on register values.

## Register coverage

Every register RM-MPU-9250A-00 section 3 lists as R/W is writable and reads
back what was written; the read-only ones and the addresses it does not list
are not writable. Reset values are 0x00 except:

| reg | name | value | why |
| --- | --- | --- | --- |
| 0x3A | `INT_STATUS` | `RAW_DATA_RDY_INT` (0x01) per sample, clear on read | RM 4.21 |
| 0x3B..0x48 | `ACCEL_*OUT`, `TEMP_OUT`, `GYRO_*OUT` | the sample | read-only |
| 0x49..0x60 | `EXT_SENS_DATA_*` | 0 | read-only; no auxiliary sensors |
| 0x6B | `PWR_MGMT_1` | 0x01 | RM section 3; bit 7 `H_RESET` restores every register and self-clears |
| 0x75 | `WHO_AM_I` | 0x71 | read-only |

While `SLEEP` is set the data registers hold; clearing it publishes the
sample at once. `PD_PTAT` holds the temperature. Awake, the worker
republishes the sample and sets `RAW_DATA_RDY_INT` every (1 + `SMPLRT_DIV`)
ms, the 1 kHz internal rate with the DLPF in use (RM 4.4). Not modelled:
the magnetometer and the auxiliary I2C master, the FIFO, `PWR_MGMT_2`'s axis
disables, the 8 kHz and 32 kHz rates without the DLPF, the offset registers'
effect, wake-on-motion, self-test, `USER_CTRL`'s `I2C_IF_DIS` and the INT
pin.

## pico-examples

`spi/mpu9250_spi` (pico-examples 0d62f75) resets the part, reads `WHO_AM_I`
and prints raw counts every 100 ms, holding chip select low for 10 ms
between the register byte and the data. It prints the temperature with the
MPU-6050's formula, `raw / 340 + 36.53`, so 31 C reads as 46.350588.
`test/pico_examples/parts.py` attaches the part on the program's pins.

## Sources

InvenSense MPU-9250 Product Specification PS-MPU-9250A-01 revision 1.0 and
Register Map and Descriptions RM-MPU-9250A-00 revision 1.4, from SparkFun's
mirror (TDK's own links no longer serve them), saved beside the other
datasheets in `.dev/resources/datasheets/` as `MPU9250REV1.0.pdf` and
`MPU-9250-Register-Map.pdf`.
