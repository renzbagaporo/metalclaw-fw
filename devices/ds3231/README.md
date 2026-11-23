<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# ds3231

Maxim's DS3231 real-time clock on I2C, as the model `ds3231`: registers
00h-12h as a register map the harness serves from hardware, a clock that
counts seconds from the harness's time base, both alarms, and the INT/SQW
pin (data sheet 19-5170 Rev 10).

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, 0x68)` |
| 1 | `int` | `gpio_out` | no | INT/SQW, open drain and active low: the DUT pulls it up |

```python
import metalclaw
rtc = metalclaw.attach("ds3231", "rtc", i2c=(4, 5, 0x68), int=(15,))
rtc.temperature = 31250   # m C, as the next conversion reads it
rtc.alarms                # (alarm 1 matches, alarm 2 matches)
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `temperature` | i32 | rw | m C | the die temperature in 11h-12h, 0.25 C steps, 25 C at attach |
| `alarms` | u32 x2 | ro | | how many times each alarm matched since attach |

## Behaviour

- Power-on (attach, and `reset(True)`): 2000-01-01 00:00:00, day 1, in 24-hour
  mode; control 1Ch (INTCN set), status 88h (OSF and EN32kHz set).
  `reset()` keeps everything, as the part runs on its battery.
- The time registers advance once a second in BCD, in 12- or 24-hour mode,
  through month lengths, leap years (every fourth, as the part counts to
  2100), the year and the century bit. A seconds write restarts the
  countdown chain: the next second comes a second after it.
- Each second the alarms are tested on the new time by Table 2's mask bits
  and DY/DT; a match sets A1F or A2F, which only a bus write of 0 clears.
- INT/SQW with INTCN set is low while an alarm flag and its enable are
  both set. With INTCN clear and RS 00 it carries the 1 Hz square wave:
  low from each second's update, high half a second on, as the data sheet
  times it from a seconds write. A control or status write moves the pin
  at once, as the part does on the write's acknowledge; Zephyr's driver
  arms its edge interrupt just after such a write and takes the next edge
  as a second's.
- CONV loads `temperature` into 11h-12h and clears.

## Not modelled

The 1.024, 4.096 and 8.192 kHz square waves (the pin stays high), the
32 kHz output, the aging offset's effect, the secondary time buffers a
START latches (a read that straddles a second can mix two), the pointer's
wrap from 12h to 00h, the 64-second automatic conversion and BSY.
