<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# mma8451q

An NXP MMA8451Q three-axis 14-bit accelerometer on I2C. The host sets
acceleration in milli-g; in active mode the model publishes it as 14-bit
two's complement counts, left-justified over each axis's MSB and LSB
registers, at the full scale `XYZ_DATA_CFG` selects.

The model fills one 256-byte `struct mclw_regmap` and arms the I2C target
engine with it. With `CTRL_REG1.F_READ` set a burst reads only the MSBs
(0x00, 0x01, 0x03, 0x05): a pointer map sends those addresses to a copy of
the four bytes laid out after the registers. Its `on_write` hook handles
`CTRL_REG1` (active, fast read), `XYZ_DATA_CFG` and `CTRL_REG2.RST`.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from the tuple `(sda, scl, address)` |

The address is 0x1C with SA0 low, 0x1D with it high (the Adafruit board's
default, which `mma8451_i2c.c` uses).

```python
import metalclaw
acc = metalclaw.attach("mma8451q", "acc", i2c=(4, 5, 0x1D))
acc.acceleration = (500, -250, 2000)   # mg
acc.reset()
acc.detach()
```

## Properties

| name | type | unit | range | meaning |
| --- | --- | --- | --- | --- |
| `acceleration` | i32[3] | mg | -8 000 .. 8 000 | X, Y, Z; saturates at the selected range |

Default (0, 0, 1000) mg. Counts are the nearest LSB at 4096, 2048 or 1024
counts/g for +-2, 4 or 8 g. There are no regions and no faults. `reset()`
restores the default and the power-on registers.

## Register coverage

| reg | name | value | why |
| --- | --- | --- | --- |
| 0x00 | `STATUS` | 0x0F once active | `XDR`..`ZYXDR` |
| 0x01..0x06 | `OUT_X_MSB..OUT_Z_LSB` | the sample, in active mode | read-only |
| 0x0B | `SYSMOD` | 0 standby, 1 wake | read-only |
| 0x0C, 0x10, 0x12, 0x16, 0x1E, 0x22 | `INT_SOURCE`, `PL_STATUS`, `PL_COUNT`, `FF_MT_SRC`, `TRANSIENT_SRC`, `PULSE_SRC` | 0 | read-only |
| 0x0D | `WHO_AM_I` | 0x1A | read-only |
| 0x11 | `PL_CFG` | 0x80 | writable |
| 0x13, 0x14 | `PL_BF_ZCOMP`, `P_L_THS_REG` | 0x44, 0x84 | read-only, as the sibling's table marks them |
| 0x09..0x31, the rest | setup and control | 0 | writable, inert except as above |

Going active publishes the sample at once; standby holds it. `CTRL_REG2.RST`
restores every register. The output data rate, oversampling modes,
auto-sleep, the high-pass filter, offsets, the FIFO, embedded functions and
the INT1/INT2 pins are not modelled, and a burst that runs past `OUT_Z_LSB`
goes on to 0x07 where the part wraps to 0x00.

## Sources

NXP no longer serves the MMA8451Q data sheet: nxp.com answers "Not found"
for `docs/en/data-sheet/MMA8451Q.pdf` on every host tried. What this model
needed comes from vendor documents for the family, saved in
`.dev/resources/datasheets/`:

- the MMA8452Q data sheet, Rev. 10, April 2016 (`MMA8452Q_rev10.pdf`), the
  12-bit sibling with the same map: Table 11 (addresses, defaults,
  auto-increment with `F_READ`), `STATUS`, `SYSMOD` (Table 13),
  `XYZ_DATA_CFG` (Table 15), `CTRL_REG1` (Table 53), `CTRL_REG2` (Table 57);
- AN4076 Rev. 1, February 2012, "Data Manipulation and Basic Settings of the
  MMA8451, 2, 3Q" (`AN4076_mma845xq_rev1.pdf`): section 2 (14-bit counts per
  g at each range), section 7 (14-bit left-justified two's complement);
- NXP's MCUXpresso driver for this part, nxp-mcuxpresso/mcux-sdk 7ba15d9,
  `components/mma8451q/fsl_mma.h`: `kMMA8451_WHO_AM_I_Device_ID` 0x1A and
  `F_SETUP` at 0x09.
