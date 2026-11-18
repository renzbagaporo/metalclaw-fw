<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# hts221

ST's HTS221 humidity and temperature sensor on I2C, as the model `hts221`:
Table 15's registers as a register map the harness serves from hardware,
with fixed calibration coefficients and outputs computed from them (data
sheet DocID026333 Rev 4, `HTS221.pdf`, the copy chipsmall hosts, which
lacks page 26). Table 19's coefficient layout, on that page, is taken from
ST's hal_st `hts221_reg.h` and Zephyr's driver, which agree: 30h-3Fh, the
T0 and T1 MSBs in 35h bits 1-0 and 3-2.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, 0x5f)` |

```python
import metalclaw
rh = metalclaw.attach("hts221", "rh", i2c=(4, 5, 0x5f))
rh.temperature = 23500   # m C
rh.humidity = 47500      # thousandths of %RH
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `temperature` | i32 | rw | m C | 25 C at attach |
| `humidity` | i32 | rw | m%RH | 50 %RH at attach |

## Behaviour

- WHO_AM_I BCh; AV_CONF 1Bh, CTRL_REG1-3 and STATUS 00h at power-on.
- The coefficients: H0 33.0 %RH at count 0, H1 75.0 %RH at 8400, T0
  20.0 C at count 0, T1 35.0 C at 1200. A sample is the count section 8's
  interpolation reads back as the property, rounded.
- With PD set, ODR 1, 7 or 12.5 Hz (Table 17) takes a sample each period;
  ONE_SHOT takes one and clears. A sample writes HUMIDITY_OUT and
  TEMP_OUT and sets H_DA and T_DA. BOOT clears itself.
- Bit 7 of the register address (SUB(7), 5.1.1) asks for auto-increment;
  the register is the other seven bits.
- `reset()` is a power cycle.

## Not modelled

The DRDY pin, the flags' clearing when the OUT_H registers are read, a
read without SUB(7) staying on one register (it runs on), BDU, the
averaging, the heater, and SPI.
