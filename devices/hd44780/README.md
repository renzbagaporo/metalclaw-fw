<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# hd44780

A Hitachi HD44780U dot-matrix character LCD behind an NXP/TI PCF8574 I2C
port expander, as one model: `hd44780_pcf8574`, a sink. Each byte the host
writes to the PCF8574 sets its port; the port drives the LCD's pins, and
the LCD latches a write at each fall of E. The host reads the DDRAM and the
CGRAM as regions and the controller's settings as properties.

The backpack wiring is the common "LCM1602 IIC" one, which pico-examples'
`lcd_1602_i2c.c` assumes in its bit constants: P0 RS, P1 R/W, P2 E, P3 the
backlight, P4-P7 DB4-DB7, DB0-DB3 open.

## Datasheets

HD44780U (LCD-II), ADE-207-272(Z) '99.9 Rev 0.0 (`HD44780U_ADE-207-272.pdf`),
and TI PCF8574 SCPS068K, revised September 2024 (`pcf8574_scps068k.pdf`).
NXP's PCF8574 data sheet could not be downloaded (its site refused the
request); TI's describes the same part. Page numbers below are the
HD44780U's; section numbers the PCF8574's.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C sink, from `(sda, scl, address)` |

The PCF8574's address is `0x20` to `0x27`, by A2-A0 (7.3.2).

```python
import metalclaw
lcd = metalclaw.attach("hd44780_pcf8574", "lcd", i2c=(4, 5, 0x27))
ddram = lcd.read_region("ddram", 0, 128)
ddram[0x00:0x10], ddram[0x40:0x50]      # the two visible lines of a 16 x 2
lcd.display_on, lcd.two_lines, lcd.backlight
```

## Properties

All read-only.

| name | type | meaning |
| --- | --- | --- |
| `port` | u32 | the PCF8574's output latch |
| `backlight` | bool | P3 |
| `display_on`, `cursor`, `blink` | bool | display on/off control D, C, B |
| `two_lines`, `font_5x10`, `eight_bit` | bool | function set N, F, DL |
| `increment`, `shift_on_write` | bool | entry mode set I/D, S |
| `address` | u32 | the address counter |
| `cgram` | bool | the counter addresses CGRAM, not DDRAM |
| `shift` | u32 | the display shift, in positions to the left |
| `instructions`, `writes` | u32 | instructions executed and RAM writes since attach |

## Regions

| name | size | access | content |
| --- | --- | --- | --- |
| `ddram` | 128 | ro | the DDRAM by its 7-bit address: one line 00h-4Fh, two lines 00h-27h and 40h-67h (pp. 10-12) |
| `cgram` | 64 | ro | the CGRAM by its 6-bit address |

## Behaviour

- **PCF8574** (7.3.1): the first data byte of a write transfer goes to the
  port; further bytes in the same transfer are ignored. At power-on the
  I/Os are high (section 3). A read returns the port.
- **Write cycle** (write operation timing, p. 49; figure 25): when E falls
  with R/W low, the LCD takes RS and DB7-DB0 as they were while E was high.
  With R/W high it was a read cycle, which the model does not answer.
- **4-bit transfers** (p. 22, figure 9): with DL 0 the high nibble comes
  first on DB7-DB4, then the low one. In 8-bit mode the open DB3-DB0 read
  1, through the pins' pull-up MOS (p. 54, note 5), which is what the 0x3,
  0x3, 0x3, 0x2 nibbles of the 4-bit initialisation (figure 24, p. 46)
  carry with them; a function set that changes DL starts the next transfer
  on a high nibble, so the sequence realigns the nibbles whatever state a
  restarted program found the part in.
- **Instructions** (table 6, pp. 24-25; pp. 26-29): clear display (fills
  the DDRAM with 20h, address 0, I/D set, S kept, shift 0), return home,
  entry mode set, display on/off control, cursor or display shift, function
  set, set CGRAM address, set DDRAM address, and data writes to whichever
  RAM the counter addresses. The counter runs on per I/D: with two lines
  from 27h to 40h and from 67h to 00h, with one from 4Fh to 00h (pp. 10-11);
  CGRAM addresses wrap at 3Fh. With S set a DDRAM write shifts the display.
- **Power-on** (internal reset, p. 23): display clear, 8-bit, one line, 5 x 8
  dots, display, cursor and blink off, increment, no shift.

## Not modelled

- The busy flag and the address counter read (RS 0, R/W 1), RAM reads, and
  the execution times: the model is never busy, and nothing is driven back
  onto the port, so a read of the PCF8574 returns what was written.
- The datasheet's note that function set cannot change N and F after the
  initialisation (p. 27): every function set takes all three bits.
- What the panel shows: the regions are the RAMs; the shift and the cursor
  are properties for the host to apply. The character generator ROM.
- The PCF8574's interrupt output and its inputs.
