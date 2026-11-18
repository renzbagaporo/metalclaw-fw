<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# sx1509b

Semtech's SX1509B 16-I/O expander with LED driver on I2C, as the model
`sx1509b`: Table 13's configuration registers 00h-7Fh as a register map
the harness serves from hardware, with the RegReset software reset. The
register map is the SX1507/SX1508/SX1509 data sheet's (Rev 1, 30 Oct.
2009, `SX1507_SX1508_SX1509.pdf`, as SparkFun hosts it); the SX1509B's own
data sheet was not at hand, and Zephyr's `semtech,sx1509b` driver uses the
same addresses (its intensity table matches Table 13's RegIOn).

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 3Eh, 3Fh, 70h or 71h by ADDR[1:0] (4.5) |

```python
import metalclaw
io = metalclaw.attach("sx1509b", "expander", i2c=(4, 5, 0x3e))
io.direction, io.data, io.led_driver, io.intensity_low
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `direction` | u32 | ro | RegDirB:RegDirA, I/O 15-0 (1 input) |
| `data` | u32 | ro | RegDataB:RegDataA as written |
| `led_driver` | u32 | ro | RegLEDDriverEnableB:A |
| `intensity_low` | u32 x8 | ro | I/O 0-7's RegIOn (ON intensity) |
| `intensity_high` | u32 x8 | ro | I/O 8-15's RegIOn |

## Behaviour

- Table 13's defaults: RegDir, RegData, RegInterruptMask and RegKeyData FFh
  in both banks, every RegIOn FFh, the rest 00h. Every register takes
  writes; addresses run on.
- 12h then 34h written to RegReset (7Dh) restore the defaults (4.4.2).
- `reset()` is a power cycle.

## Not modelled

The I/O pins themselves: RegData reads back what was written, not pin
levels, and no output drives a pin. Also the LED engine's PWM, blink and
breathe timing, the keypad engine, the interrupt and debounce logic, level
shifting, the oscillator, and the NRESET and NINT pins.
