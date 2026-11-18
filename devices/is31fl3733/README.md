<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# is31fl3733

ISSI's (Lumissil's) IS31FL3733 12x16 dot-matrix LED driver on I2C, as the
model `is31fl3733`: its four register pages as a register map with four
banks the harness serves from hardware. Data sheet Rev. G, 10/29/2024
(`IS31FL3733.pdf`).

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 50h-5Fh by ADDR2 and ADDR1 (Table 1) |

```python
import metalclaw
matrix = metalclaw.attach("is31fl3733", "matrix", i2c=(4, 5, 0x50))
matrix.leds_on, matrix.pwm_lit, matrix.gcc
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `page` | u32 | ro | the page FDh points to |
| `configuration` | u32 | ro | page 3's 00h (SSD in bit 0: 1 for normal operation) |
| `gcc` | u32 | ro | page 3's 01h, the global current |
| `leds_on` | u32 | ro | dots switched on in page 0, of 192 |
| `pwm_lit` | u32 | ro | dots whose PWM (page 1) is not zero |
| `pwm_max` | u32 | ro | the highest PWM |

## Behaviour

- FDh selects page 0 (LED on/off), 1 (PWM), 2 (auto breath mode) or 3
  (function), but only once after C5h is written to FEh; FEh reads 00h
  again after (Table 3). FEh and F0h are in every page.
- Every register is 00h at power-on; page 0 is selected and FDh locked.
- Page 0's open (18h-2Fh) and short (30h-47h) registers read 00h.
- Addresses run on within a page.
- `reset()` is a power cycle.

## Not modelled

The LEDs themselves, the PWM and auto breath timing, the open and short
detection, the interrupt (F1h reads 00h), the reset on reading page 3's
11h, the SDB pin, and the sync modes.
