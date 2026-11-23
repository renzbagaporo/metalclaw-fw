<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# lsm6dso

ST's LSM6DSO and LSM6DSL 6-axis IMUs on I2C, as the models `lsm6dso` and
`lsm6dsl`: the main register page as a register map the harness serves from
hardware, with data-ready on INT1. Datasheets: DS12140 Rev 3 (June 2024)
and DocID028475 Rev 6 (May 2017). The two parts share the main page, sensitivities and data-ready
configuration; the LSM6DSL answers WHO_AM_I 6Ah, has no PIN_CTRL and resets
CTRL9_XL to 00h.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 6Ah with SA0 low, 6Bh high |
| 1 | `int1` | `gpio_out` | no | the INT1 pin |

```python
import metalclaw
imu = metalclaw.attach("lsm6dso", "imu", i2c=(4, 5, 0x6a), int1=15)
imu.acceleration = (0, 0, 1000)   # mg
imu.angular_rate = (0, 0, 0)      # mdps
imu.samples
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `acceleration` | i32[3] | rw | mg | what each accelerometer sample reads, (0, 0, 1000) at attach |
| `angular_rate` | i32[3] | rw | mdps | likewise for the gyroscope, zero at attach |
| `temperature` | i32 | rw | m C | 25 C at attach |
| `samples` | u32 | ro | | ticks that took a sample since attach or reset |

## Registers and sampling

The LSM6DSO's table 20 main page at its reset values (PIN_CTRL 3Fh,
WHO_AM_I 6Ch, CTRL3_C 04h, CTRL9_XL E0h; the LSM6DSL's table 19 as above); the sources, status and outputs are read-only.
SW_RESET or BOOT in CTRL3_C restores them. The accelerometer samples at
CTRL1_XL's ODR and the gyroscope at CTRL2_G's (table 44: 12.5 Hz to
6.66 kHz, and the accelerometer's 1.6 Hz), at most once a millisecond tick,
in counts of table 2's sensitivities at the full scale selected (0.061 to
0.488 mg; 4.375 to 70 mdps); the temperature at 256 LSB/C from 25 C. Each
sample sets STATUS_REG's XLDA or GDA, and TDA.

INT1 carries the data-ready flags INT1_CTRL routes (INT1_DRDY_XL,
INT1_DRDY_G): latched while they are set or, with COUNTER_BDR_REG1's
dataready_pulsed, a 75 us pulse per sample; active high unless CTRL3_C's
H_LACTIVE.

## Not modelled

The other register pages (FUNC_CFG_ACCESS's bank bits select nothing: the
sensor hub and embedded function registers read and write the main page),
the FIFO, the embedded functions (tap, step counter, tilt, FSM, MLC), the
sensor hub, interrupts other than data-ready, INT2, PP_OD, the filters and
offsets, self-test, BDU's hold and IF_INC 0. STATUS_REG's data-available
bits clear when STATUS_REG is read; the part clears them when the outputs
are read.
