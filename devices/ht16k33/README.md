<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# ht16k33

A Holtek HT16K33 RAM-mapped 16 x 8 LED controller on I2C, as a sink. Every
write transfer starts with a command byte; only the display data address
pointer takes data after it. The host reads the display RAM as a region and
the settings as properties.

## Datasheet

Holtek HT16K33 Rev 1.10, May 16, 2011 (`HT16K33_v1.10.pdf`). Page numbers
below are from it.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C sink, from `(sda, scl, address)` |

The address is `0x70` to `0x77`, by A2-A0 (slave addressing, p. 26).

```python
import metalclaw
leds = metalclaw.attach("ht16k33", "leds", i2c=(4, 5, 0x70))
leds.read_region("display", 0, 16)    # COMn: ROW0-7 at 2n, ROW8-15 at 2n + 1
leds.oscillator, leds.display_on, leds.blink, leds.dimming
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `oscillator` | bool | ro | system setup S (20h/21h) |
| `display_on` | bool | ro | display setup D (80h-87h) |
| `blink` | u32 | ro | display setup B1 B0: 0 off, 1 2 Hz, 2 1 Hz, 3 0.5 Hz |
| `dimming` | u32 | ro | dimming set P3-P0 (E0h-EFh): duty (n + 1)/16 |
| `row_int` | u32 | ro | ROW/INT set: bit 0 row/int, bit 1 act (A0h-A3h) |
| `address` | u32 | ro | the display data address pointer |

## Regions

| name | size | access | content |
| --- | --- | --- | --- |
| `display` | 16 | ro | the display RAM (p. 13): address 2n holds ROW0-7 of COMn, 2n + 1 ROW8-15 |

## Behaviour

- **Commands** (command summary, pp. 30-31): the first byte of each write
  transfer. `0xh` sets the display data address pointer and the bytes after
  it are display data; the others take no data, and further bytes after
  them are ignored. Patterns not defined change nothing (p. 30 note).
- **Page write** (p. 28): the pointer moves on after each data byte and
  wraps from 0Fh to 00h.
- **Standby** (p. 8): with the oscillator off only system setup is taken,
  commands and RAM writes are not.
- **Power-on** (p. 8): oscillator off, display off, blinking off, ROW/INT as
  ROW outputs, dimming 16/16 (EFh, p. 31). The RAM's power-on content is not
  given; attach clears it.

## Not modelled

- Reads: every byte read returns `0x00`, which is what the key data RAM and
  the INT flag hold with no key pressed. Reading the display RAM back is
  not served.
- Key scanning, the INT pin, blinking and dimming as light: the settings are
  properties, nothing changes over time.
- The 1 ms after power-on during which the part ignores the bus (p. 8).
