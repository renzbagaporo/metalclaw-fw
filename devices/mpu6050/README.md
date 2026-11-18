<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# mpu6050

An InvenSense MPU-6050 six-axis IMU on I2C. The harness answers at the
part's address with its register map; the host sets acceleration in
milli-g, angular rate in milli-degrees per second and the die temperature
in millidegrees, and the model turns them into the 16-bit big-endian counts
at the full scales the DUT selects in `ACCEL_CONFIG` and `GYRO_CONFIG`.

The model fills one 256-byte `struct mclw_regmap`, bank 0, and arms the I2C
target engine with it; the register pointer auto-increments, so a burst from
`ACCEL_XOUT_H` reads the whole sample. Its `on_write` hook handles
`PWR_MGMT_1` (reset, sleep) and the two full-scale registers.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from the tuple `(sda, scl, address)` |
| 1 | `int` | `gpio_out` | no | the INT pin |

The address is 0x68 with AD0 low, 0x69 with it high.

With `int` bound, each sample's data ready drives INT while INT_ENABLE's
DATA_RDY_EN is set (register map RM-MPU-6000A 4.15-4.16): held until
INT_STATUS clears when INT_PIN_CFG's LATCH_INT_EN is set, else a 50 us pulse
per sample.
INT_LEVEL picks active high (0) or low (1); the pin is driven push-pull at
either (INT_OPEN is not modelled).

```python
import metalclaw
imu = metalclaw.attach("mpu6050", "imu", i2c=(4, 5, 0x68))
imu.acceleration = (250, -500, 1125)       # mg
imu.angular_rate = (10000, -2000, 3000)    # mdps
imu.temperature = 26530                    # mC
imu.reset()
imu.detach()
```

## Properties

| name | type | unit | range | meaning |
| --- | --- | --- | --- | --- |
| `acceleration` | i32[3] | mg | -16 000 .. 16 000 | X, Y, Z; saturates at the selected range |
| `angular_rate` | i32[3] | mdps | -2 000 000 .. 2 000 000 | X, Y, Z; saturates at the selected range |
| `temperature` | i32 | mC | -40 000 .. 85 000 | die temperature |

Defaults: (0, 0, 1000) mg, (0, 0, 0) mdps, 25 000 mC. Counts are the nearest
LSB: 16384 LSB/g at +-2 g, halving per `AFS_SEL` step; 131 LSB/(deg/s) at
+-250 deg/s, halving per `FS_SEL` step; temperature `(T - 36.53 C) x 340`.
There are no regions and no faults. `reset()` restores the defaults and the
power-on register values.

## Register coverage

Everything in 0x00..0x75 is zero at reset and writable, except:

| reg | name | value | why |
| --- | --- | --- | --- |
| 0x3A | `INT_STATUS` | `DATA_RDY` (0x01) per sample, clear on read | read-only |
| 0x3B..0x48 | `ACCEL_*OUT`, `TEMP_OUT`, `GYRO_*OUT` | the sample | read-only |
| 0x49..0x60 | `EXT_SENS_DATA_*` | 0 | read-only; no auxiliary sensors |
| 0x6B | `PWR_MGMT_1` | 0x40 | `SLEEP` at power-up; bit 7 `DEVICE_RESET` restores every register and self-clears |
| 0x75 | `WHO_AM_I` | 0x68 | read-only |

While `SLEEP` is set the data registers hold (zero after a reset); clearing
it publishes the sample at once. `TEMP_DIS` holds the temperature. Awake,
the worker republishes the sample and sets `DATA_RDY` every
(1 + `SMPLRT_DIV`) ms, the Linux driver's 1 kHz sample clock; the 8 kHz
gyroscope rate without the DLPF is not modelled. The FIFO, the auxiliary I2C
master, motion interrupts, self-test and the INT pin are not modelled.

## Sources

TDK's MPU-6000/MPU-6050 Product Specification (PS-MPU-6000A-00) and Register
Map (RM-MPU-6000A-00) could not be downloaded: tdk.com answers 403 and
invensense.tdk.com no longer has them. Everything here comes from the
Linux driver InvenSense contributed, `drivers/iio/imu/inv_mpu6050/`
(`inv_mpu_iio.h` at torvalds/linux d23d763: register addresses, the
`PWR_MGMT_1` and `INT_STATUS` bits, `INV_MPU6050_WHOAMI_VALUE`, the FSR
shift, `INV_MPU6050_INTERNAL_FREQ_HZ`, `INV_MPU6050_TEMP_OFFSET`/`TEMP_SCALE`;
`inv_mpu_core.c`: `accel_scale`, `gyro_scale_6050` and "after reset, the
sleep bit could be on"), saved beside the datasheets in
`.dev/resources/datasheets/`, and from the temperature formula
`mpu6050_i2c.c:117` applies. The driver's `gyro_scale_6050` (133090 nrad/s
per LSB) is 131.1 LSB/(deg/s); the model uses 131, a figure not checked
against the specification. The power-up value of `PWR_MGMT_1` (0x40) is
inferred from the driver, not read from the register map.
