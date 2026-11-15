<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# max7219

Maxim MAX7219 serially interfaced 8-digit LED display drivers, one or a
cascade of up to eight, on a receive-only SPI binding: DIN, CLK and LOAD.
The model keeps the cascade's shift register and latches every part's last
16 bits when a window ends, as LOAD's rise does. The host reads the digit
registers as a region and the control registers as properties.

## Datasheet

Maxim MAX7219/MAX7221 19-4452 Rev 4, 7/03 (`MAX7219-MAX7221_19-4452_rev4.pdf`).
Page and table numbers below are from it.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `spi` | `spi_sink` | yes | a receive-only SPI target, from `(sck, mosi, cs)`: CLK, DIN, LOAD |

DIN is shifted in on CLK's rising edge, D15 first (serial addressing modes,
p. 6): modes 0 and 3. CLK is 10 MHz at most (tCP, p. 3).

```python
import metalclaw
matrix = metalclaw.attach("max7219", "matrix", spi=(18, 19, 17))
matrix.chain = 4                          # four parts on one DIN, before the program runs
matrix.read_region("digits", 0, 32)      # part k, digit d at 8k + d
matrix.intensity, matrix.scan_limit, matrix.shutdown
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `chain` | u32 | rw | parts cascaded, 1-8; setting it is a power-up of the cascade |
| `decode_mode` | u32 x 8 | ro | per part, register 9 (table 4) |
| `intensity` | u32 x 8 | ro | per part, register A, 0-15 (table 7) |
| `scan_limit` | u32 x 8 | ro | per part, register B, digits scanned - 1 (table 8) |
| `shutdown` | u32 x 8 | ro | per part, register C D0: 0 shutdown, 1 normal operation (table 3) |
| `display_test` | u32 x 8 | ro | per part, register F D0 (table 10) |
| `loads` | u32 | ro | windows latched since attach |

Part 0 is the one whose DIN the DUT drives. Entries past `chain` are 0.

## Regions

| name | size | access | content |
| --- | --- | --- | --- |
| `digits` | 64 | ro | per part, digit registers 1-8 (digit 0-7) as written; decode them per `decode_mode` with code B (table 5) |

## Behaviour

- **Serial data** (table 1, p. 6): 16-bit packets, D11-D8 the register
  address (table 2), D7-D0 the data, D15-D12 ignored.
- **Cascade** (p. 6, no-op register p. 10): DIN goes through each part's
  16-bit register to DOUT and the next part's DIN; at LOAD's rise every part
  latches what is in its register. After a window of `chain` words, part 0
  holds the last one and the last part the first. A shorter window leaves
  the words it did not push through where they were, and they are latched
  again; no-op words (address 0) latch nothing.
- **Registers**: digits, decode mode, intensity (low 4 bits), scan limit (low
  3), shutdown and display test (D0) take the word's data.
- **Power-up** (initial power-up, p. 7): every control register reset, so
  shutdown, no decode, one digit, minimum intensity. The digit registers'
  power-up content is not given; the model starts them at 0.

## Not modelled

- Bits clocked while LOAD is high. The MAX7219 shifts them in as well
  (p. 6); the engine captures only while chip select is low, as the
  MAX7221 does.
- DOUT (the engine drives nothing), the display itself, multiplexing,
  blanking in shutdown and display test, and the 250 µs to leave shutdown.
