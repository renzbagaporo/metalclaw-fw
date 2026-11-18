<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# lps22hb

ST's LPS22HB MEMS nano pressure sensor on I2C, as the model `lps22hb`: a
register map the harness serves from hardware (datasheet DocID027083 Rev 5,
`LPS22HB-st.pdf`, as SparkFun hosts it: ST's site does not serve it to a
script).

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 5Ch with SA0 low, 5Dh high |

```python
import metalclaw
baro = metalclaw.attach("lps22hb", "baro", i2c=(4, 5, 0x5c))
baro.pressure = 100000   # Pa
baro.temperature = 23500 # m C
baro.conversions
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `pressure` | i32 | rw | Pa | what a conversion reads, 101325 at attach |
| `temperature` | i32 | rw | m C | likewise, 25 C at attach |
| `conversions` | u32 | ro | | conversions since attach or reset |

## Registers

Table 16 at its reset values: WHO_AM_I B1h, CTRL_REG2 10h (IF_ADD_INC),
the rest zero; WHO_AM_I, INT_SOURCE, FIFO_STATUS, STATUS and the outputs
are read-only. SWRESET or BOOT in CTRL_REG2 resets them.

## Conversion

CTRL_REG1's ODR runs conversions at 1, 10, 25, 50 or 75 Hz on the
simulation clock, which ticks every millisecond; at ODR 000 (power-down)
ONE_SHOT takes one and clears itself. A conversion writes PRESS_OUT (24
bits, 4096 LSB/hPa) and TEMP_OUT (16 bits, 100 LSB/C), little-endian from
the low byte, and sets STATUS's P_DA and T_DA.

## Not modelled

The FIFO, the pressure threshold interrupt and INT_SOURCE, the reference
pressure and offset, the low-pass filter, BDU's hold (outputs update
whenever a conversion lands), IF_ADD_INC 0 (a transfer always runs on to
the next address), the data-available bits clearing on read, and the
DRDY/INT pin.
