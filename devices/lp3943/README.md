<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# lp3943

TI's LP3943 16-LED fun-light driver on I2C, as the model `lp3943`:
Table 6-1's registers 00h-09h as a register map the harness serves from
hardware. Data sheet SNVS256E (`LP3943.pdf`).

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 60h-67h by ADR2-ADR0 (6.5.2) |

```python
import metalclaw
leds = metalclaw.attach("lp3943", "leds", i2c=(4, 5, 0x60))
leds.selectors, leds.pwm
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `selectors` | u32 | ro | LS3:LS2:LS1:LS0, two bits per LED with LED0 in bits 1-0: 00 off, 01 on, 10 DIM0, 11 DIM1 |
| `pwm` | u32 x4 | ro | PSC0, PWM0, PSC1, PWM1 |

## Behaviour

- Tables 6-4 to 6-11's defaults: PWM0 and PWM1 80h, the rest 00h.
- The register address byte's bit 4 turns auto increment on and bits 3-0
  pick the register (6.5.4); the model runs on through the map either way.
- Input 1 and Input 2 (00h, 01h) take no writes. They read each output's
  level as its selector sets it: 0 for an LED switched on (the output
  sinks), 1 otherwise.
- `reset()` is a power cycle.

## Not modelled

The outputs themselves (no pin is driven), the DIM0 and DIM1 blink and
PWM timing (a dimmed output reads 1), auto increment's wrap from 09h to
02h on writes, the RST pin, and a DUT driving an output low from outside.
