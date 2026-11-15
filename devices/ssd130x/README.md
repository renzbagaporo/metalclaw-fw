<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# ssd130x

Solomon Systech SSD1306 and SSD1309 128 x 64 OLED controllers on I2C or
4-wire SPI, two models from one decoder: `ssd1306` and `ssd1309`. Each is a
sink: in serial mode the controller only takes writes (8.1.3), so the
engine hands the model every byte and the model decodes commands and data
into its graphic display data RAM, which the host reads as a region.

## Datasheets

Solomon Systech SSD1306 Advance Information Rev 1.1, Apr 2008, with its
charge pump application note Rev 0.4 appended (`SSD1306_rev1.1.pdf`), and
SSD1309 Advance Information Rev 1.1, Jul 2011 (`SSD1309_rev1.1.pdf`).
Section and table numbers below are from them; both number the command
tables 9-1 to 9-5.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | one of `i2c`, `spi` | an I2C sink, from `(sda, scl, address)` |
| 1 | `spi` | `spi_sink` | one of `i2c`, `spi` | a receive-only SPI target, from `(sck, mosi, cs, dc)` |
| 2 | `reset` | `gpio_in` | no | RES# |

The I2C address is `0x3C` or `0x3D`, by SA0 (8.1.5.2). On SPI, SDIN is
shifted in on SCLK's rising edge MSB first and D/C# is sampled at the
eighth (8.1.3, table 8-4): modes 0 and 3. Attaching both or neither of `i2c`
and `spi` is refused, as BS[2:0] select one interface (8.1).

```python
import metalclaw
oled = metalclaw.attach("ssd1306", "oled", i2c=(4, 5, 0x3C))
oled = metalclaw.attach("ssd1309", "oled", spi=(18, 19, 17, 20), reset=21)
oled.read_region("gddram", 0, 1024)     # page p, column c at 128p + c
oled.display_on, oled.addressing_mode, oled.contrast
```

## Properties

All read-only; what the program last set.

| name | type | meaning |
| --- | --- | --- |
| `display_on` | bool | AEh/AFh |
| `addressing_mode` | u32 | 20h: 0 horizontal, 1 vertical, 2 page |
| `column`, `page` | u32 | the RAM write pointer |
| `column_range`, `page_range` | u32 x 2 | 21h and 22h: start and end |
| `contrast` | u32 | 81h |
| `inverse` | bool | A6h/A7h |
| `entire_on` | bool | A4h/A5h |
| `segment_remap` | bool | A0h/A1h |
| `com_remap` | bool | C0h/C8h |
| `multiplex` | u32 | A8h: the MUX ratio, N + 1 |
| `display_offset`, `start_line` | u32 | D3h, 40h-7Fh |
| `com_pins`, `clock`, `precharge`, `vcomh` | u32 | DAh, D5h, D9h, DBh as written |
| `scroll` | bool | 2Fh activated, 2Eh deactivated |
| `data_bytes`, `commands` | u32 | RAM writes and commands taken since attach |
| `charge_pump` (`ssd1306`) | bool | 8Dh A[2] (application note, table 1) |
| `locked` (`ssd1309`) | bool | FDh A[2] (10.22) |

## Regions

| name | size | access | content |
| --- | --- | --- | --- |
| `gddram` | 1024 | ro | 8 pages of 128 columns; row 8p + n in bit n of page p's byte (8.7) |

## Behaviour

- **I2C framing** (8.1.5.2): after the address, a control byte with Co in
  bit 7 and D/C# in bit 6. Co 0: every later byte of the transfer is of
  that kind. Co 1: one byte is, and the next is a control byte again.
- **SPI framing** (8.1.3): D/C# low, a command byte; high, a RAM byte.
  Chip select framing does not matter to the part; each byte stands alone.
- **Commands** (tables 9-1 to 9-5, 10.1): every command with its parameter
  bytes; the parser waits for all of them, across transfers, before it
  acts. 26h/27h take 6 parameters on the SSD1306 and 7 on the SSD1309,
  29h/2Ah 5 and 7, the SSD1309's 2Ch/2Dh 7. A pattern not in the tables is
  one byte that changes nothing.
- **Addressing** (10.1.3, figures 10-1, 10-3, 10-4): horizontal and
  vertical modes run through the 21h/22h window and wrap to its start; page
  mode runs along the page from the 00h-1Fh start column and wraps to it at
  column 127. 21h and 22h also move the pointer to their start (10.1.4).
- **RES#** (8.5 / 8.4): low resets every register to its table value and
  holds the part, RAM kept. The model samples the pin before each delivery
  and in its poll, and latches a fall, so a pulse shorter than a delivery
  is still seen. Attach is a power-on: RAM zeroed (its content is not
  specified), registers reset.
- **Command lock** (SSD1309, 10.22): `FDh 16h` locks; locked, the part takes
  nothing but `FDh 12h`, and no RAM writes.
- Reset values that differ: D5h is 80h on the SSD1306 and 70h on the
  SSD1309 (table 9-5 in its Rev 1.1), DBh 20h and 34h.

## Not modelled

- Scrolling and content scroll move the panel's picture and, on the real
  part, the RAM (10.2.3: the RAM needs rewriting after 2Eh). The model takes
  the setup commands and the `scroll` flag and leaves the RAM as written.
- What the panel shows: the region is the RAM; remap, offset, start line,
  inverse and entire-on are properties for the host to apply.
- Reads (6800/8080 parallel status and data reads); the serial interfaces
  have none. An I2C read of the model returns `0xFF`.
- The parallel interfaces, the 3-wire SPI mode, timing and power.
