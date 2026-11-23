<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# pca9633

NXP's PCA9633 4-bit Fm+ I2C-bus LED driver, as the model `pca9633`: a
register map the harness serves from hardware (data sheet Rev. 05, 25 July
2008). It is the RGB backlight of Seeed's Grove LCD RGB
Backlight, at 62h.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from the tuple `(sda, scl, address)` |

```python
import metalclaw
leds = metalclaw.attach("pca9633", "leds", i2c=(4, 5, 0x62))
leds.brightness, leds.ledout, leds.sleep, leds.writes
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `brightness` | u32[4] | ro | what LED0..LED3 show, 0..255: LEDOUT's off (0), on (255), PWMx, or PWMx dimmed by GRPPWM (7.3.6) |
| `ledout` | u32 | ro | LEDOUT |
| `sleep` | bool | ro | MODE1's SLEEP; no LED shows PWM while it is set |
| `writes` | u32 | ro | register writes since attach or reset |

## Registers

00h..0Ch as table 7 gives them, at their reset values (tables 8-15):
MODE1 91h, MODE2 05h, PWM0..PWM3 0, GRPPWM FFh, GRPFREQ 0, LEDOUT 0,
SUBADR1..3 E2h/E4h/E8h, ALLCALLADR E0h. The control register's D[3:0]
selects one; MODE1's AI2..AI0 are read-only and MODE2's bits 7:6 read 0.

## Not modelled

The Auto-Increment options (a transfer runs on through 00h..0Ch whatever
AI2..AI0 say, without their roll-overs), blinking (DMBLNK shows PWMx), the
subaddresses and All Call address, the oscillator's wake-up time, OCH, and
the outputs' electrical modes.
