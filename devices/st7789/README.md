<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# st7789

A Sitronix ST7789V 240RGB x 320 TFT controller on its 4-line serial
interface, as a sink: `st7789v`. The model decodes commands and memory
writes. The frame memory, 240 x 320 x 18 bits (172 kB), does not fit the
harness, so the model keeps every eighth column of every eighth row, a
30 x 40 thumbnail, and counts what it is sent.

## Datasheet

Sitronix ST7789V Version 1.3, 2014/03 (`ST7789V_v1.3.pdf`). Section and page
numbers below are from it.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `spi` | `spi_sink` | yes | a receive-only SPI target, from `(sck, mosi, cs, dc)`: SCL, SDA, CSX, D/CX |
| 1 | `reset` | `gpio_in` | no | RESX |

SDA is sampled on SCL's rising edge, MSB first, and D/CX at the eighth
(8.4.2, pp. 56-57): modes 0 and 3. A write cycle is 66 ns at least
(TSCYCW, table 6, p. 44), about 15 MHz; the `spi_sink` engine is good for
10 MHz.

```python
import metalclaw
tft = metalclaw.attach("st7789v", "tft", spi=(18, 19, 17, 20), reset=21)
tft.read_region("thumbnail", 0, 2400)   # pixel (8x, 8y) at 2 (30y + x), RGB 5-6-5, big-endian
tft.colmod, tft.madctl, tft.column_range, tft.pixels
```

## Properties

All read-only.

| name | type | meaning |
| --- | --- | --- |
| `sleeping` | bool | SLPIN/SLPOUT (10h/11h) |
| `display_on` | bool | DISPOFF/DISPON (28h/29h) |
| `inversion` | bool | INVOFF/INVON (20h/21h) |
| `idle` | bool | IDMOFF/IDMON (38h/39h) |
| `partial` | bool | PTLON/NORON (12h/13h) |
| `colmod` | u32 | COLMOD (3Ah) |
| `madctl` | u32 | MADCTL (36h) |
| `column_range`, `row_range` | u32 x 2 | CASET (2Ah) and RASET (2Bh): start and end |
| `memory_writes` | u32 | RAMWR commands since attach |
| `pixels` | u32 | pixels written since attach |

## Regions

| name | size | access | content |
| --- | --- | --- | --- |
| `thumbnail` | 2400 | ro | frame memory pixel (8x, 8y), x < 30, y < 40, as RGB 5-6-5 big-endian; 18-bit pixels keep their top bits |

## Behaviour

- **Framing** (8.4.2, p. 56): D/CX low, a command; high, the command's next
  parameter, or frame memory data after RAMWR (2Ch) or WRMEMC (3Ch). Chip
  select does not frame anything.
- **Memory writes** (9.1.22, p. 202; 9.1.33): RAMWR starts at the CASET and
  RASET window's start, WRMEMC goes on from where the last write stopped.
  The pointer runs along the column window, then down the row window, and
  wraps to the start; pixels outside the 240 x 320 memory are dropped
  (9.1.20, p. 198). Pixels are 16 bits in two bytes with COLMOD's control
  format 101b, 18 bits in three otherwise (8.8.42, 8.8.43); 12-bit pixels
  (011b) are counted and not stored.
- **Resets**: power-on and RESX low (7.4.5) take the registers to their
  defaults (9.1): sleeping, display off, inversion and idle off, normal
  mode, COLMOD 66h, MADCTL 0, columns 0-239, rows 0-319. SWRESET (01h,
  9.1.2) does the same but keeps COLMOD and MADCTL (9.1.28, 9.1.32) and,
  with MV set, swaps the window's ends (9.1.20, 9.1.21). The frame memory is
  kept by both resets (p. 202); attach clears it.

## Not modelled

- MADCTL's address orders (8.12): the thumbnail is in the write window's
  coordinates, as the host addressed it.
- Reads, the RGB and parallel interfaces, the 3-line serial interface,
  gamma, porch, power and every other command of table 2 (9.2): their
  parameters arrive and are dropped.
- Frame memory outside the thumbnail's grid.

## pico-examples

`pio/st7789_lcd` drives this part from a two-instruction PIO program at
`SERIAL_CLK_DIV` 1, one bit per two system clocks: 62.5 MHz on a Pico,
75 MHz on a Pico 2, beyond both the part's 15 MHz and the `spi_sink`
engine, which needs about four instructions a bit (`core/ports/rp2/hal/
spi_sink.c`). The program also uses GP2 and GP3, which on a stacked Pico
harness are the SWD pins and reach nothing. So no rig case attaches it.
