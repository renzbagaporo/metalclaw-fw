<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# tmp1xx

Texas Instruments digital temperature sensors on I2C, three models of one
register shape: `tmp108` (SBOS663A), `tmp112` (SBOS473) and `tmp116`
(SNOSD68). Each register is 16 bits, MSB first, behind its pointer value;
the harness serves them from hardware.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from the tuple `(sda, scl, address)` |
| 1 | `alert` | `gpio_out` | no | the ALERT pin, active low unless POL |

The TMP108 answers at 48h-4Bh, the TMP112 at 48h-4Bh, the TMP116 at
48h-4Bh, by the ADD0 strap.

```python
import metalclaw
t = metalclaw.attach("tmp112", "thermo", i2c=(4, 5, 0x48))
t.temperature = 23500   # m C
t.conversions
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `temperature` | i32 | rw | m C | what the next conversion measures, 25 C at attach |
| `conversions` | u32 | ro | | conversions since attach or reset |

## Registers

| part | registers (pointer: reset) |
| --- | --- |
| `tmp108` | temperature 0, configuration 1: 2210h, TLOW 2, THIGH 3 (no reset value given; 0) |
| `tmp112` | temperature 0, configuration 1: 60A0h, TLOW 2: 4B00h, THIGH 3: 5000h |
| `tmp116` | temperature 0, configuration 1: 0220h, THIGH 2: 6000h, TLOW 3: 8000h, EEPROM_UL 4, EEPROM1-4 5-8 (a fixed pattern), DEVICE_ID Fh: 1116h |

The temperature is 12 bits of 0.0625 C, left-justified (the TMP112 13
bits with EM, bit 0 set), or the TMP116's 16 bits of 7.8125 m C. The bits
a datasheet marks read-only keep the part's value when the bus writes
them.

## Conversions

A write to the configuration register restarts the schedule, on the
simulation clock, which ticks every millisecond:

- `tmp108`: M1:M0 00 shutdown, 01 one conversion of 27 ms, after which M
  reads 00, 1x continuous at CR's 0.25, 1, 4 or 16 Hz.
- `tmp112`: continuous at CR's 0.25, 1, 4 or 8 Hz, 26 ms to the first;
  with SD set, OS starts one conversion, OS reading 0 until it ends.
- `tmp116`: MOD continuous (00, 10), shutdown (01), or one-shot (11, then
  01), at the conversion cycle time of CONV and AVG; each result sets
  Data_Ready.

Each result compares the temperature with the limits in comparator mode:
the TMP108's FH and FL with its hysteresis on the way back, the TMP112's AL
from THIGH until TLOW, the TMP116's HIGH_Alert and LOW_Alert; ALERT
follows, at POL.

## Not modelled

Interrupt (thermostat) mode, the TMP112's fault queue, the read side
effects (the TMP108 flags and the TMP116 Data_Ready and alert flags do not
clear when the configuration register is read), the SMBus alert response,
general-call reset, the TMP116's EEPROM programming time and EEPROM_Busy,
and the TMP116's offset register (TMP117).
