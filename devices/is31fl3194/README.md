<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# is31fl3194

ISSI's (Lumissil's) IS31FL3194 3-channel LED driver on I2C, as the model
`is31fl3194`: Table 1's registers 00h-4Fh as a register map the harness
serves from hardware. Data sheet Rev. C, 03/27/2024 (`IS31FL3194.pdf`).

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 53h, the part's only address |

```python
import metalclaw
rgb = metalclaw.attach("is31fl3194", "rgb", i2c=(4, 5, 0x53))
rgb.levels, rgb.updates
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `configuration` | u32 | ro | 01h: SSD in bit 0 (1 for normal operation), the LED mode in bits 2:1 |
| `current_band` | u32 | ro | 03h |
| `levels` | u32 x3 | ro | OUT1, OUT2, OUT3 (10h, 21h, 32h) as the last color update latched them |
| `updates` | u32 | ro | color updates: C5h written to 40h |

## Behaviour

- Table 1's defaults: product ID CEh at 00h (read-only), output enable
  07h, current band 15h, each pattern's color enable and NXT 01h, the
  rest 00h.
- C5h to the Color Update Register (40h) latches the current level
  registers into `levels`; C5h to the Reset Register (4Fh) restores the
  defaults.
- Addresses run on.
- `reset()` is a power cycle.

## Not modelled

The LEDs themselves, pattern mode and its timing (the pattern update
registers 41h-43h are taken and do nothing), the hold function, and the
SDB pin.
