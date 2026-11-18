<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# mpu9250

An InvenSense MPU-9250 accelerometer and gyroscope on SPI or I2C. The host
sets acceleration in milli-g, angular rate in milli-degrees per second and
the die temperature in millidegrees, and the model turns them into the
16-bit big-endian counts at the full scales the DUT selects in
`ACCEL_CONFIG` and `GYRO_CONFIG`. The AK8963 magnetometer in the same
package answers behind the MPU-9250's auxiliary I2C master: the model has
one of its own (register map section 5; AKM MS1356-E-02, `AK8963C-250842.pdf`,
a copy that lacks its own register chapter), reached by slave 4's single
transfers and slave 0's periodic reads.

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
| 2 | `int` | `gpio_out` | no | the INT pin |

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
ms, the 1 kHz internal rate with the DLPF in use (RM 4.4).

The auxiliary I2C master, with `USER_CTRL`'s `I2C_MST_EN` set: writing
`I2C_SLV4_CTRL` with `I2C_SLV4_EN` runs slave 4's single transfer at once
(RM 4.13-4.17), a read of the AK8963 at 0Ch into `I2C_SLV4_DI` or a write
from `I2C_SLV4_DO`, then clears `I2C_SLV4_EN` and sets `I2C_MST_STATUS`'s
`I2C_SLV4_DONE` (`I2C_SLV4_NACK` for any other address); at every sample,
slave 0 reads `I2C_SLV0_LENG` bytes from the AK8963 at `I2C_SLV0_REG` into
`EXT_SENS_DATA_00` on (RM 4.11-4.12). The AK8963 answers WIA 48h and its
fuse ROM's ASAX..ASAZ with 128 (an adjustment of exactly 1), measures the
`magnetic_field` property in 16 bits of 0.15 uT or 14 bits of 0.6 uT
(CNTL1's BIT) once in single mode or at 8 or 100 Hz in the continuous
modes, sets ST1's DRDY (DOR when unread) and clears them when ST2 is read;
CNTL2's SRST resets it.

With `int` bound, `INT_ENABLE`'s `RAW_RDY_EN` makes each sample emit a
50 us pulse on INT, or with `INT_PIN_CFG`'s `LATCH_INT_EN` hold it until
`INT_STATUS` is read, at `ACTL`'s level, push-pull.

Not modelled: the auxiliary master's slaves 1-3, its delays and pass-through
mode, the AK8963's self-test and overflow, the FIFO, `PWR_MGMT_2`'s axis
disables, the 8 kHz and 32 kHz rates without the DLPF, the offset registers'
effect, wake-on-motion, self-test, `USER_CTRL`'s `I2C_IF_DIS` and the INT
pin's open-drain mode.

## pico-examples

`spi/mpu9250_spi` (pico-examples 0d62f75) resets the part, reads `WHO_AM_I`
and prints raw counts every 100 ms, holding chip select low for 10 ms
between the register byte and the data. It prints the temperature with the
MPU-6050's formula, `raw / 340 + 36.53`, so 31 C reads as 46.350588.
`test/pico_examples/parts.py` attaches the part on the program's pins.
The program starts the `WHO_AM_I` read 0.7 us after the reset write's chip
select rises and clocks its first bit 3.2 us later (timed on fixture 1),
well after the 0.25 us the SPI target takes to be running for the next
window.

## Sources

InvenSense MPU-9250 Product Specification PS-MPU-9250A-01 revision 1.0 and
Register Map and Descriptions RM-MPU-9250A-00 revision 1.4, from SparkFun's
mirror (TDK's own links no longer serve them), saved beside the other
datasheets in `.dev/resources/datasheets/` as `MPU9250REV1.0.pdf` and
`MPU-9250-Register-Map.pdf`.
