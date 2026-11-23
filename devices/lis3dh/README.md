<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# lis3dh

An ST LIS3DH three-axis accelerometer on I2C, with its auxiliary ADC's
temperature channel. The host sets acceleration in milli-g and the
temperature in millidegrees; the model turns them into the left-justified
16-bit outputs at the resolution (low-power 8 bits, normal 10, high
resolution 12) and full scale the DUT configures, in the byte order `BLE`
selects.

The model fills one 256-byte `struct mclw_regmap` and arms the I2C target
engine with it. A pointer map sends every register address with bit 7 set
(the SUB auto-increment bit drivers set for a burst) to the same register,
so `0xA8` starts a burst at `OUT_X_L`. Its `on_write` hook republishes the
sample when `CTRL_REG1`, `CTRL_REG4` or `TEMP_CFG_REG` change and clears
`CTRL_REG5.BOOT`.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from the tuple `(sda, scl, address)` |

The address is 0x18 with SDO/SA0 low, 0x19 with it high.

```python
import metalclaw
acc = metalclaw.attach("lis3dh", "acc", i2c=(4, 5, 0x18))
acc.acceleration = (252, -500, 1020)   # mg
acc.temperature = 28750                # mC
acc.reset()
acc.detach()
```

## Properties

| name | type | unit | range | meaning |
| --- | --- | --- | --- | --- |
| `acceleration` | i32[3] | mg | -16 000 .. 16 000 | X, Y, Z; saturates at the 16-bit output |
| `temperature` | i32 | mC | -40 000 .. 85 000 | what the temperature channel measures |

Defaults: (0, 0, 1000) mg and 25 000 mC. Digits are the nearest step: 16,
32, 64 or 192 mg per digit in low-power mode, 4, 8, 16 or 48 in normal mode,
1, 2, 4 or 12 in high-resolution mode, for +-2, 4, 8 or 16 g. The
temperature channel reads the change from 25 C, one digit per C in 8 bits
(low power) or four per C in 10 bits, left-justified. There are no regions
and no faults. `reset()` restores the defaults and zeroes every register.

## Register coverage

| reg | name | value | why |
| --- | --- | --- | --- |
| 0x07 | `STATUS_REG_AUX` | 0x0F once the ADC is on | `1DA`..`321DA` |
| 0x08..0x0D | `OUT_ADC1..3` | ADC3 the temperature with `TEMP_EN`, else 0 | read-only |
| 0x0F | `WHO_AM_I` | 0x33 | read-only |
| 0x1E..0x26 | `CTRL_REG0`, `TEMP_CFG_REG`, `CTRL_REG1..6`, `REFERENCE` | 0 | writable |
| 0x27 | `STATUS_REG` | 0x0F once powered | `XDA`..`ZYXDA` |
| 0x28..0x2D | `OUT_X_L..OUT_Z_H` | the sample | read-only |
| 0x2E, 0x30, 0x32..0x34, 0x36..0x38, 0x3A..0x3F | FIFO, interrupt, click, activity setup | 0 | writable, inert |
| 0x2F, 0x31, 0x35, 0x39 | `FIFO_SRC_REG`, `INT1_SRC`, `INT2_SRC`, `CLICK_SRC` | 0 | read-only |

`CTRL_REG1.ODR` zero is power-down: the outputs hold. The status bits stay
set; reading the outputs does not clear them, the FIFO, interrupts, click
and activity detection are not modelled, nor is the INT1/INT2 pin. Bursts
without bit 7 auto-increment as well, which the part may not.

## Sources

The register addresses, bit fields, `LIS3DH_ID` and the
data scaling come from ST's driver, STMicroelectronics/lis3dh-pid at
8c0588b: `lis3dh_reg.h` (register map, `lis3dh_ctrl_reg1_t`,
`lis3dh_ctrl_reg4_t`, `lis3dh_temp_cfg_reg_t`, the status registers) and
`lis3dh_reg.c` (`lis3dh_from_fs*_{lp,nm,hr}_to_mg`,
`lis3dh_from_lsb_*_to_celsius`, `lis3dh_aux_adc_set`). The bit-7 auto-increment rule is
Zephyr v4.4.0's `LIS2DH_AUTOINCREMENT_ADDR` (`drivers/sensor/st/lis2dh/
lis2dh.h`). The driver does not give reset values, so every register but
`WHO_AM_I` starts at zero here; the datasheet's own reset values were not
checked.
