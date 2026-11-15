<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# lcd_backpack

Adafruit's USB + Serial RGB backlight character LCD backpack (product 781)
on its TTL serial input, with the 16x2 or 20x4 character panel it drives.
The DUT sends it text and Matrix Orbital style commands, each `0xFE` and a
command byte with its arguments; the model keeps what the panel shows, the
cursor, the backlight and everything the backpack saves in its EEPROM, and
the host reads them back.

The model is one file with no bus code: it parses what the UART engine
delivers on the worker, a byte at a time, so a command split across
deliveries is taken whole.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `uart` | `uart` | yes | a UART, from the tuple `(tx, rx)` |

9600 baud, 8N1 by default (guide, "Testing with TTL Serial"). The backpack
only listens; the one thing it says is its baud rate, for command `0xD2`.

```python
import metalclaw
lcd = metalclaw.attach("lcd_backpack", "lcd", uart=(9, 8))
lcd.columns, lcd.rows, lcd.cursor, lcd.contrast, lcd.brightness, lcd.color
lcd.read_region("display", 0, 16), lcd.read_region("display", 20, 16)   # rows 1 and 2
lcd.read_region("splash", 0, 16)
lcd.reset()                 # a power cycle; reset(True) is the factory state
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `columns`, `rows` | u32 | ro | the panel size, set by `0xD1` |
| `cursor` | u32[2] | ro | column and row of the text cursor, from 1 as `0x47` counts |
| `contrast` | u32 | ro | 0 .. 255, set by `0x50` or `0x91` |
| `brightness` | u32 | ro | 0 .. 255, set by `0x99` or `0x98` |
| `color` | u32[3] | ro | red, green, blue of the backlight, set by `0xD0` |
| `backlight` | bool | ro | on after `0x42`, a brightness or a colour, off after `0x46` |
| `autoscroll` | bool | ro | `0x51` on, `0x52` off |
| `underline_cursor`, `block_cursor` | bool | ro | `0x4A` and `0x53` turn one on; `0x4B` or `0x54` turns both off |
| `gpo`, `gpo_start` | u32 | ro | bit n is general purpose output n + 1, now and at power-up (`0x56`, `0x57`, `0xC3`) |
| `baud` | u32 | rw | the rate the backpack listens at; `0x39` changes it, and a set is what a previous owner saved |
| `commands`, `characters` | u32 | ro | commands and text bytes taken since attach or reset |

| region | size | meaning |
| --- | --- | --- |
| `display` | 80 | what the panel shows, four rows of 20 bytes; a 16x2 panel uses the first 16 of the first two |
| `splash` | 80 | the start-up screen `0x40` saved, laid out as `display`; 0xFF bytes are the firmware's built-in splash |
| `custom` | 64 | the eight custom characters, eight bytes each (`0x4E`, `0xC0`) |
| `banks` | 320 | the five EEPROM banks of eight custom characters (`0xC1`) |

At attach the backpack is in the factory state its firmware's factory block
writes: 16x2, contrast 200, brightness 255, white, built-in splash, 9600
baud, autoscroll and both cursors off, the screen clear and the backlight
on. `reset()` is a power cycle: the screen clears and everything saved in
EEPROM (size, contrast, brightness, colour, autoscroll, splash, baud, banks,
GPO start states) stays; `reset(True)` restores the factory state. There
are no faults.

## Text and commands

A byte other than `0xFE` is text: it is written at the cursor, which moves
on, past the last column to the next row, past the last row to the top
left, or with autoscroll on every row moves up and the bottom one is
blanked. `\r` fills the rest of the row with spaces and moves to the next,
`\n` is ignored, `\b` blanks the previous cell and leaves the cursor
there. The guide has the roles of `\r` and `\n` the other way round; the
firmware does as above, and so does the model.

| command | arguments | effect |
| --- | --- | --- |
| `0x42` display on | 1, ignored | backlight on |
| `0x46` display off | | backlight off |
| `0x99`, `0x98` brightness | 1 | saved; backlight on |
| `0x50`, `0x91` contrast | 1 | saved |
| `0x51`, `0x52` autoscroll | | saved |
| `0x58` clear | | screen blank, cursor home |
| `0x40` splash | columns x rows | saved, row by row |
| `0x47` cursor | column, row | from 1; a column past the end runs into the rows below, a row past the end wraps |
| `0x48` home | | cursor to (1, 1) |
| `0x4C`, `0x4D` back, forward | | one cell, wrapping at either end |
| `0x4A`, `0x4B`, `0x53`, `0x54` | | cursors, as above |
| `0x39` baud | 1 | 0x53 1200, 0x29 2400, 0xCF 4800, 0x67 9600, 0x33 19200, 0x22 28800, 0x19 38400, 0x10 57600, 0x08 115200; another code changes nothing |
| `0x4E` custom character | slot, 8 bytes | into slot % 8 |
| `0xC1` save custom character | bank, slot, 8 bytes | into EEPROM; a bank above 4 is ignored |
| `0xC0` load custom bank | bank | all eight; a bank above 4 is ignored |
| `0x56`, `0x57` GPO off, on | output 1 .. 4 | |
| `0xC3` GPO start state | output, state | saved |
| `0xD0` RGB | red, green, blue | saved; backlight on |
| `0xD1` size | columns, rows | saved and in effect at once, as the firmware does; held to 1 .. 20 by 1 .. 4 |
| `0xD2` test baud | | sends the baud rate in decimal |
| anything else | | ignored |

Not modelled: the second of splash and the baud rate the backpack shows at
power-up, the start-up banner its firmware prints on the serial line, the
USB side, the colour mixing and PWM of the backlight, and how the HD44780
draws custom characters.

## pico-examples

`uart/lcd_uart` (pico-examples 0d62f75) sets the backpack up on uart1, TX
on GP8, at 9600: backlight on (it sends the byte at address 0 as the
ignored argument), 16x2, contrast 155, brightness 255, both cursors off,
the splash "Hello LCD, from Pi Towers!" padded to 32, home and clear. Then
it forwards what arrives on its console, changing the backlight colour per
character. It prints nothing, so `test/pico_examples/parts.py` judges it by
the part's state.

## Sources

Adafruit's "USB + Serial RGB Backlight Character LCD Backpack" guide (PDF of
learn.adafruit.com/usb-plus-serial-backpack, last updated 2025-07-28),
"Command Reference", and the backpack's firmware
`Adafruit_USBSerialLCD.ino` at adafruit/Adafruit-USB-Serial-RGB-Character-Backpack
9b8b221, both in `.dev/resources/datasheets/`.
