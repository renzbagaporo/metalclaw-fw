<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# lp50xx

TI's LP5012 12-channel RGB LED driver on I2C, as the model `lp5012`: Table
8-4's registers 00h-17h as a register map the harness serves from hardware
(data sheet SLVSEH2B, LP5009 and LP5012, `LP5012.pdf`). Zephyr drives it as
`ti,lp5012`.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 14h-17h by ADDR1-ADDR0 (Table 8-3) |

```python
import metalclaw
leds = metalclaw.attach("lp5012", "leds", i2c=(4, 5, 0x14))
leds.enabled      # Chip_EN
leds.brightness   # LED0-LED3_BRIGHTNESS
leds.led0         # OUT0-OUT2_COLOR, module 0's R, G, B as wired
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `enabled` | bool | ro | DEVICE_CONFIG0's Chip_EN |
| `brightness` | u32 x4 | ro | LED0-LED3_BRIGHTNESS |
| `led0`-`led3` | u32 x3 | ro | each RGB module's three OUTx_COLOR registers |
| `writes` | u32 | ro | register writes since attach |

## Behaviour

- Table 8-4's defaults: DEVICE_CONFIG1 3Ch (Log_Scale_EN, Power_Save_EN,
  Auto_Incr_EN, PWM_Dithering_EN), BANK_BRIGHTNESS and LED0-LED3_BRIGHTNESS
  FFh, the rest 00h.
- Writing FFh to RESET (17h) restores the defaults (8.6.24); RESET reads
  00h.
- `reset()` is a power cycle.

## Not modelled

The PWM outputs themselves (8.3.1.3's product of color and brightness, the
logarithmic curve, dithering), bank control, LED_Global_Off, the EN pin,
the broadcast address, power-save and the protection features.
