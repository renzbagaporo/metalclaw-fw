<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# square_wave

A square wave on one pin: what a signal generator puts on a DUT input. A
clock, a button pressed and let go at a steady rate, a wake signal, or,
behind an RC filter, a PWM that the filter turns into a voltage.

A period under 2 ms comes from the port's hardware and runs in real time
(on rp2 a PWM slice with an integer divider, 16 ns to about 134 ms at
125 MHz). A longer period follows the simulation clock, at its 1 ms step: it
stops while the clock is paused and steps with `advance_us()`. Every change
of a property starts the period again, high first.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `out` | `gpio_out` | yes | the pin, driven push-pull |

```python
import metalclaw
wake = metalclaw.attach("square_wave", "wake", out=15)
wake.period = 1_000_000_000      # 1 s
wake.high = 900_000_000          # high for 900 ms of it
clock = metalclaw.attach("square_wave", "clock", out=20)
clock.period = 21333             # 46.875 kHz, in hardware
clock.high = 10667
wake.period = 0                  # a steady level from here
wake.level = True
```

## Properties

| name | type | unit | range | meaning |
| --- | --- | --- | --- | --- |
| `period` | u32 | ns | 0 .. 4 294 967 295 | the wave's period; 0 for a steady level |
| `high` | u32 | ns | 0 .. 4 294 967 295 | how long it is high in each period; at least the period holds it high |
| `level` | bool | | | the steady level while `period` is 0 |
| `hardware` | bool | | read only | whether the port's hardware makes the wave |

Defaults: `period` 0, `high` 0, `level` low. A period the hardware cannot
make is refused with `OSError` (`ERANGE`) and the wave stays as it was; so
is a second pin on a PWM slice whose other pin has another period
(`EBUSY`). The hardware makes the nearest wave it can: 21 333 ns is 2667
cycles of 125 MHz, 46 869 Hz. `reset()` restores the defaults. There are no
regions and no faults.
