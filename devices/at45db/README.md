<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# at45db

Renesas (Adesto) AT45DB041E 4-Mbit DataFlash on SPI, in its "power of 2"
binary page size, as the model `at45db041e`: a command image the harness
serves from hardware. Data sheet DS-AT45DB041E-8783 Rev. Q
(`AT45DB041E.pdf`).

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `spi` | `spi_target` | yes | an SPI target, from `(sck, mosi, miso, cs)`, modes 0 and 3 (4) |

```python
import metalclaw
flash = metalclaw.attach("at45db041e", "flash", spi=(18, 19, 16, 17))
flash.held_base = 0xfe00
flash.programs, flash.erases, flash.power_down
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `power_down` | bool | ro | in Deep Power-Down (B9h or 79h) |
| `reads`, `programs`, `erases` | u32 | ro | array reads, page programs and read-modify-writes, erases |
| `lost` | u32 | ro | programmed bytes outside the held pages |
| `ignored` | u32 | ro | commands ignored |
| `last_ignored` | u32 x2 | ro | the last one's opcode, then why: 1 in Deep Power-Down, 2 malformed, 3 unknown |
| `held_base` | u32 | rw | the first held byte, a page boundary; the 7 pages from it are held |
| `spi_mode` | u32 | rw | the SPI mode served |

The `array` region is the held pages, from `held_base`.

## Behaviour

- Continuous Array Read, low power (01h), from the held pages; a read runs
  on through them and reads FFh past them.
- Status Register Read (D7h): ready, density 0111, binary page size
  (Table 20), served on every byte. From the release of a program or erase
  until the model has done it, RDY/BUSY reads busy.
- Main Memory Byte/Page Program through Buffer 1 (02h) clears bits of the
  bytes clocked in; Read-Modify-Write (58h, 59h) replaces them (7.5, 7.6).
  Both wrap within the page.
- Page (81h), Block (50h), Sector (7Ch, with sectors 0a and 0b) and Chip
  (C7h 94h 80h 9Ah) Erase leave FFh.
- Manufacturer and Device ID (9Fh): 1Fh 24h 00h, then EDI 01h 00h
  (Table 23).
- Deep Power-Down (B9h) and Ultra-Deep Power-Down (79h) ignore everything
  but Resume (ABh) (11).
- `reset()` is a power cycle and keeps the array; a factory reset erases
  it.

## Not modelled

The standard 264-byte page size (the binary page size command, 3Dh 2Ah 80h
A6h, is accepted and changes nothing), the buffers as such and their
commands, the higher-speed read opcodes (0Bh, 03h, 1Bh, D2h), program and
erase times, sector protection and lockdown, the security register, and
the RESET and WP pins.
