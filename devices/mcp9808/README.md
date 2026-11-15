<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# mcp9808

A Microchip MCP9808 digital temperature sensor on I2C. The harness answers at
the part's address with its 16-bit register file; the host sets the ambient
temperature in millidegrees, and the DUT reads it from `TA` with the three
comparison bits against the limits it wrote.

The MCP9808's registers are 16 bits wide, MSB first, and selected by a
pointer byte; it does not read or write sequentially, and a read with no
pointer returns the register last selected (DS25095A 4.1.1). The image
holds register `p` at bytes `2p` and `2p + 1`, and the model gives the map
a pointer map (`regmap.h`) that sends pointer `p` to byte `2p`, so the I2C
engine starts every transaction at the selected register's MSB.

## Datasheet

Microchip MCP9808 data sheet DS25095A, 2011 (`25095A.pdf`). Section,
register and table numbers below are from it.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)` |

The address is `0011 A2 A1 A0`, `0x18` with the address pins low
(DS25095A 4.1.4, table 3-2).

```python
import metalclaw
thermo = metalclaw.attach("mcp9808", "thermo", i2c=(4, 5, 0x18))
thermo.temperature = 34562
thermo.upper, thermo.lower, thermo.critical   # what the DUT wrote, in mC
thermo.resolution, thermo.config
thermo.reset()
```

## Properties

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `temperature` | i32 | rw | mC | -40 000 .. 125 000 | the ambient temperature, the part's operating range |
| `upper` | i32 | ro | mC | | `TUPPER` as the DUT wrote it |
| `lower` | i32 | ro | mC | | `TLOWER` |
| `critical` | i32 | ro | mC | | `TCRIT` |
| `resolution` | u32 | ro | | 0 .. 3 | the Resolution register: 0 is 0.5 degC .. 3 is 0.0625 degC |
| `config` | u32 | ro | | 0 .. 0xFFFF | `CONFIG` |

`temperature` defaults to 25 000 mC. `reset()` restores it and the
power-on register values, locks included.

## Behaviour

- `TA` (register 5-4): the temperature rounded to the nearest 0.0625 degC,
  13-bit two's complement, the bits a coarser Resolution setting does not
  resolve cleared (register 5-7, note 2 of register 5-4). Bits 15, 14 and 13
  are `TA >= TCRIT`, `TA > TUPPER` and `TA < TLOWER`, recomputed whenever the
  temperature or a limit or the resolution changes.
- `TUPPER`, `TLOWER`, `TCRIT` (register 5-3): writable, bits 15..13 and 1..0
  read 0.
- `CONFIG` (register 5-2): writable; `Int Clr` and `Alert Stat` read 0. The
  `Crit. Lock` and `Win. Lock` bits stay set until a power-on reset and make
  `TCRIT`, or `TUPPER` and `TLOWER`, read-only. `SHDN` stops conversions: the
  temperature `TA` shows is held until it is cleared.
- Resolution (register 5-7): one byte, writable, bits 1..0.
- A conversion is immediate rather than every tCONV (table 5-2).

| pointer | register | power-on | access | from |
| --- | --- | --- | --- | --- |
| 0x0 | RFU | `0x001F` | ro | table 5-1 |
| 0x1 | `CONFIG` | `0x0000` | rw | register 5-2, table 5-3 |
| 0x2 | `TUPPER` | `0x0000` | rw | register 5-3 |
| 0x3 | `TLOWER` | `0x0000` | rw | register 5-3 |
| 0x4 | `TCRIT` | `0x0000` | rw | register 5-3 |
| 0x5 | `TA` | the temperature | ro | register 5-4 |
| 0x6 | Manufacturer ID | `0x0054` | ro | register 5-5 |
| 0x7 | Device ID / revision | `0x0400` | ro | register 5-6 |
| 0x8 | Resolution | `0x03` | rw | register 5-7 |

Any other pointer reads `0x00`.

## Not modelled

- The Alert pin and everything that drives it: comparator and interrupt
  modes, polarity, the hysteresis (`THYST`, figure 5-10) on the comparison
  bits, and `Int Clr`. The locks protect the limit registers but not the
  `CONFIG` bits register 5-2 also freezes under them.
- tCONV: a new temperature or resolution shows in `TA` at once.
- The SMBus time-out (4.1.7) and reads past a register's last byte, which
  continue into the next register's bytes.
