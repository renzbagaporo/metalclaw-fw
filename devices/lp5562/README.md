<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# lp5562

TI's LP5562 4-channel RGBW LED driver on I2C, as the model `lp5562`: the
registers of 7.6, 00h-70h, at their reset values, a register map the
harness serves from hardware (data sheet SNVS820B, `LP5562.pdf`). Zephyr
drives it as `ti,lp5562`.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 30h-33h by ADDR_SEL (Table 24, write addresses 60h-66h) |

```python
import metalclaw
leds = metalclaw.attach("lp5562", "leds", i2c=(4, 5, 0x30))
leds.enabled, leds.pwm, leds.led_map
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `enabled` | bool | ro | ENABLE's CHIP_EN |
| `pwm` | u32 x4 | ro | R, G, B and W PWM |
| `led_map` | u32 | ro | LED MAP (70h): which engine or I2C drives each output |
| `op_mode` | u32 | ro | OP MODE: the engines' modes |
| `writes` | u32 | ro | register writes since attach |

## Behaviour

- 7.6's reset values: B, G, R and W CURRENT AFh (17.5 mA), LED MAP 39h,
  the rest 00h, program memory 10h-6Fh included. Every register takes
  writes.
- FFh in RESET (0Dh) restores them (7.6.14); RESET reads 00h.
- `reset()` is a power cycle.

## Not modelled

The three program engines: their memory keeps what is written but no
program runs, so the engine-driven outputs (blink) and ENGx_INT do not
change. Also the outputs' PWM and current, the 32 kHz clock detection,
power save and the EN pin.
