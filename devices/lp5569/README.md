<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# lp5569

TI's LP5569 9-channel LED driver on I2C, as the model `lp5569`: the
registers of 8.6.1, 00h-4Fh, at their reset values, a register map the
harness serves from hardware (data sheet SNVSAP8A, `LP5569.pdf`). Zephyr
drives it as `ti,lp5569`.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 32h-35h by ADDR_SEL0-1 (Table 2) |

```python
import metalclaw
leds = metalclaw.attach("lp5569", "leds", i2c=(4, 5, 0x32))
leds.enabled                    # CONFIG's CHIP_EN
leds.pwm_low, leds.pwm_high     # LED0-LED4_PWM, LED5-LED8_PWM
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `enabled` | bool | ro | CONFIG's CHIP_EN |
| `pwm_low` | u32 x5 | ro | LED0-LED4_PWM |
| `pwm_high` | u32 x4 | ro | LED5-LED8_PWM |
| `writes` | u32 | ro | register writes since attach |

## Behaviour

- 8.6.1's reset values: LEDx_CURRENT AFh, MISC 40h, MISC2 02h,
  ENGINE_STATUS 80h, IO_CONTROL 02h, the rest 00h. Every register in
  00h-4Fh takes writes.
- Writing FFh to RESET (3Fh) restores them (Table 54); RESET reads 00h.
- `reset()` is a power cycle.

## Not modelled

The three program engines and their memory, the master faders, the
charge pump, the outputs' PWM and current, the TRIG, INT, CLK and EN
pins, and the start-up and engine-busy status bits.
