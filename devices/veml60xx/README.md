<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# veml60xx

Vishay light sensors on I2C, two models: `veml6031`, the VEML6031X00
ambient light and IR sensor (`veml6031x00.pdf`, document 80007), and
`veml6046`, the VEML6046X00 red, green, blue and IR sensor
(`veml6046x00.pdf`, 80173). Each has byte registers behind 8-bit command
codes, which the harness serves from hardware; a transfer runs on through
consecutive codes.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 29h (or 10h for the -10 versions) |

```python
import metalclaw
light = metalclaw.attach("veml6031", "light", i2c=(4, 5, 0x29))
light.counts = (1200, 300, 0, 0)   # ALS, IR (VEML6031); R, G, B, IR (VEML6046)
light.measurements
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `counts` | u32[4] | rw | the raw counts a measurement reads, per channel, 1000 at attach |
| `measurements` | u32 | ro | measurements since attach or reset |

## Registers

Table 2 of each: CONF_0 (01h, shut down) and CONF_1 (80h) at 00h and 01h,
the thresholds at 04h-07h, the data words little-endian from 10h (ALS and
IR; R, G, B and IR), the ID word 0001h (14h, VEML6031; 18h, VEML6046), and
INT_FLAG (16h-17h; 1Ah-1Bh), which clears when read. The data and ID are
read-only.

## Measurement

CONF_0's SD shuts the part down. In active force mode (AF) each TRIG takes
one measurement of the integration time, 3.125 ms x 2^IT, after which TRIG
clears and INT_FLAG's high byte sets its data-ready bit (bit 3); without AF
the part measures continuously, a result every integration time. Every
measurement reads `counts`, on the simulation clock, which ticks every
millisecond.

## Not modelled

Gain, the photodiode size and DIV4 (the counts are what the host sets,
whatever they say), the threshold interrupts and their persistence, the
interrupt pin, and the calibration bit.
