<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# lsm303dlhc

The magnetometer of ST's LSM303DLHC on I2C, as the model `lsm303dlhc_magn`:
registers 00h-0Ch and 31h-32h of Table 17 as a register map the harness
serves from hardware (data sheet Doc ID 018771 Rev 1, `LSM303DLHC.pdf`,
as Adafruit hosts it, section 7.2). The accelerometer, at 19h, is a
separate I2C target and not modelled; Zephyr drives the magnetometer as
`st,lsm303dlhc-magn`.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, 0x1e)` |

```python
import metalclaw
m = metalclaw.attach("lsm303dlhc_magn", "magn", i2c=(4, 5, 0x1e))
m.magnetic_field = (25000, -10000, 50000)   # nT on X, Y, Z
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `magnetic_field` | i32 x3 | rw | nT | X, Y, Z; 0 at attach |
| `temperature` | i32 | rw | m C | 25 C at attach, for TEMP_OUT with TEMP_EN set |

## Behaviour

- Power-on: CRA_REG_M 10h, CRB_REG_M 20h, MR_REG_M 03h (sleep); IRA-IRC
  48h, 34h, 33h.
- A measurement writes X, Z and Y, high byte first, at Table 75's gain for
  GN2-GN0 (counts rounded, held to F800h-07FFh), TEMP_OUT at 8 LSB/C
  left-justified when TEMP_EN is set, and DRDY.
- Continuous-conversion mode measures at once and then at Table 72's rate;
  single-conversion mode measures once and returns to sleep. The data sheet
  gives only minimum rates, not when the first set lands; Zephyr's
  magn_polling reads DRDY as soon as its driver has set the mode, and lists
  the stm32f3_disco's LSM303DLHC.
- `reset()` is a power cycle.

## Not modelled

DRDY clearing as the outputs are read, LOCK, the DRDY pin, the
accelerometer.
