<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# is31fl3216a

ISSI's (Lumissil's) IS31FL3216A 16-channel LED driver on I2C, as the model
`is31fl3216a`: Table 2's registers as a register map the harness serves
from hardware, with PWM data loaded into the outputs by the Update
Register (data sheet Rev. B, 02/24/2025, `IS31FL3216A.pdf`). Zephyr drives
it as `issi,is31fl3216a`.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 74h, 76h or 75h with AD to GND, VCC or SCL (Table 1) |

```python
import metalclaw
leds = metalclaw.attach("is31fl3216a", "leds", i2c=(4, 5, 0x74))
leds.on, leds.out_low, leds.out_high, leds.updates
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `on` | bool | ro | out of software shutdown (Configuration's SSD clear) |
| `out_low` | u32 x8 | ro | OUT1-OUT8's duty as last loaded |
| `out_high` | u32 x8 | ro | OUT9-OUT16's duty as last loaded |
| `updates` | u32 | ro | writes to the Update Register since attach |

## Behaviour

- Table 2's defaults: Configuration 80h (software shutdown), both LED
  Control Registers FFh, the rest 00h. Every register takes writes;
  addresses run on (address auto increment).
- The PWM Registers (10h-1Fh: OUT16 at 10h, OUT1 at 1Fh, Table 13) hold
  their data until a write to the Update Register (B0h) loads all sixteen
  into the outputs.
- `reset()` is a power cycle.

## Not modelled

The LED Control Registers' enables and shutdown applied to the outputs
(the duties read as loaded), the lighting effects, audio modulation and
AGC, the frames and Auto Frame Play, the GPIO and input-port modes and
their interrupt, the State Register, and the SDB pin.
