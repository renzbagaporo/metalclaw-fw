<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# pcf8523

An NXP PCF8523 real-time clock on I2C. The harness answers at `0x68` with
the part's twenty registers; its time counters run on the harness's
simulation clock, one increment per second, and the DUT sets them by
writing them as it would the part. The host can read and set the time, and
the alarm flag rises when the counters reach the alarm the DUT set.

## Datasheet

NXP PCF8523 product data sheet, Rev. 7, 28 April 2015 (`PCF8523.pdf`).
Section and table numbers below are from it.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)` |

The part's address is `0x68` (8.11.5, table 46). Registers auto-increment after
each byte (8.1).

```python
import metalclaw
rtc = metalclaw.attach("pcf8523", "rtc", i2c=(4, 5, 0x68))
rtc.time = (26, 11, 15, 0, 12, 34, 56)   # 2026-11-15, Sunday, 12:34:56
rtc.time                                 # read back; it counts on
rtc.af, rtc.stop, rtc.os
metalclaw.pause(); metalclaw.advance_us(60_000_000); rtc.time
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `time` | i32 x 7 | rw | | year (0..99), month, day, weekday (0..6, 0 Sunday), hour (0..23), minute, second |
| `stop` | bool | ro | | `Control_1.STOP`: the counters are frozen |
| `os` | bool | ro | | `Seconds.OS`: clock integrity not guaranteed (set at power-on, cleared by the DUT) |
| `af` | bool | ro | | `Control_2.AF`: the alarm has triggered |

`time` is decoded from the BCD registers whichever hour mode `Control_1`
selects, so a read returns what the DUT wrote even when that is not a valid
date (pico-examples' `pcf8523_i2c` writes day 0 and month 0). A write must be
a valid date; it keeps `OS` and restarts the second, so the next increment
is a whole second later. Attach is a power-on: the counters are undefined
there (table 10) and the model starts them at 00-01-01, a Saturday,
00:00:00, with `OS` set.

## Behaviour

- **Counting** (8.6.8, figure 13): every second of the simulation clock the
  counters increment, with carries through minutes, hours, days, months and
  years; February has 29 days when the year counter is divisible by 4,
  00 included (8.6.4); the weekday counts 0..6 beside the day. A paused
  clock stops the RTC; `advance_us` counts the seconds it covers in one go.
- **12/24 hour** (table 15): with `12_24` set the Hours register holds
  1..12 and `AMPM`; the model counts and encodes that way.
- **STOP** (8.10): while set the counters do not move; after it is cleared
  the first increment comes 0.5 s later (table 45), then one per second.
- **Software reset** (8.3): writing `58h` to `Control_1` resets the control
  registers, `Offset`, `Tmr_CLKOUT_ctrl` and the timer frequency
  registers, sets `OS` and every alarm's `AEN` bit, and leaves the time
  (table 10).
- **Alarm** (8.7.5): when a minute increment brings every enabled alarm
  field (`AEN_x` = 0) to equal its counter, and they did not all match
  before it, `AF` is set; it stays set until cleared. Alarm fields compare
  in the counters' own coding, so a 12 hour alarm compares `AMPM` too.
- **Flag writes** (8.7.5): writing `Control_2` can only clear `CTAF`, `CTBF`,
  `SF` and `AF` (a written 1 keeps the flag); the interrupt enables are
  plain bits. `WTAF` reads 0.
- **Unimplemented bits** read 0 (table 6), and `Control_1.SR` always reads 0
  (table 7).

| reg | name | power-on | access |
| --- | --- | --- | --- |
| 00h | `Control_1` | `0x00` | rw; `58h` is the software reset |
| 01h | `Control_2` | `0x00` | flags clear on a written 0; enables rw |
| 02h | `Control_3` | `0xE0` | rw: `PM[2:0]`, `BSIE`, `BLIE` |
| 03h..09h | `Seconds`..`Years` | 00-01-01 Saturday 00:00:00, `OS` set | rw, counting |
| 0Ah..0Dh | alarms | `AEN` set | rw |
| 0Eh | `Offset` | `0x00` | rw, stored only |
| 0Fh..13h | clock-out and timers | `0x00`; `Tmr_A_freq_ctrl`, `Tmr_B_freq_ctrl` `0x07` | rw, stored only |

## Not modelled

- The counters do not freeze during an I2C access (8.6.8); they move only
  on the simulation clock's tick, between bus bytes, so a burst that reads
  across a carry can see the two sides of it.
- The address wrap from 13h to 00h (figure 6): reads past 13h return `0x00`.
- Timers A and B, the second interrupt, the watchdog, clock-out, the
  offset correction, the battery switch-over and battery-low detection, and
  the INT1/INT2 pins: their registers hold what the DUT writes and nothing
  happens. `BSF` and `BLF` stay 0.
- A long `advance_us` counts on a copy of the registers; if the DUT writes
  the time meanwhile, its write wins and the count is dropped.
