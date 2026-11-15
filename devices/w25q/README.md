<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# w25q

Winbond W25Q serial NOR flash on SPI, as the `w25q16jv` model: the
W25Q16JV, 16 Mbit. The DUT erases, programs and reads it with the part's
standard SPI instructions and polls its status register while it is busy;
the host reads and writes the array as a programmer would.

The model is a command image (`metalclaw/command.h`). Read Data (03h) is
the image's memory read, served from hardware with no model call: the
opcode, three address bytes, and the array from that address. Every other
instruction is answered with the status register on every byte, and reaches
the model after chip select rises, where it is executed: Read Status
Register-1 (05h) therefore reads the status, and everything that changes
the part does so in the model.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `spi` | `spi_target` | yes | an SPI target, from the tuple `(sck, mosi, miso, cs)` |

SPI modes 0 and 3, MSB first (datasheet 6.1). The part reads at up to
50 MHz (9.6, fR); the harness's memory read is published at 2 MHz, an
estimate not yet measured on the bench (`docs/devices.md`).

```python
import metalclaw
flash = metalclaw.attach("w25q16jv", "flash", spi=(18, 19, 16, 17))
flash.write_region("array", 0, b"\x00\x01\x02")   # as a programmer left it
flash.read_region("array", 0, 16)
flash.status, flash.busy, flash.programs, flash.erases, flash.lost
flash.reset()             # a power cycle; reset(True) also erases the array
```

## Properties and regions

| name | type | access | meaning |
| --- | --- | --- | --- |
| `status` | u32 | ro | Status Register-1 as the part holds it: BUSY bit 0, WEL bit 1 (7.1) |
| `write_enabled`, `busy` | bool | ro | WEL, BUSY |
| `power_down` | bool | ro | after B9h, until ABh |
| `jedec_id` | u32 | ro | 0xEF4015 (8.1, Manufacturer and Device Identification); not served on the bus, see below |
| `reads`, `programs`, `erases` | u32 | ro | Read Data, Page Program and erase instructions taken |
| `lost` | u32 | ro | bytes a Page Program cleared outside the held array, which keeps nothing there |
| `ignored` | u32 | ro | instructions the part ignored: busy, powered down, no WEL, malformed, or unknown |
| `spi_mode` | u32 | rw | 0 or 3 |

| region | size | meaning |
| --- | --- | --- |
| `array` | 1792 | the first seven 256-byte pages, rw; writing it publishes it |

The harness holds the array's first 1792 bytes, the seven pages the SPI
target's command image can serve; every other address reads FFh, erased,
and keeps nothing a Page Program writes there. At attach the held pages
are erased. `reset()` is a power cycle: the array stays, WEL, BUSY and
power-down clear, and the counters start again; `reset(True)` also erases
the held pages. There are no faults.

## Instructions

| instruction | effect |
| --- | --- |
| 06h Write Enable, 04h Write Disable | WEL set, cleared |
| 05h Read Status Register-1 | answered from the image |
| 03h Read Data | answered from the image |
| 02h Page Program | with WEL: the data goes into the addressed page from the given byte on, wrapping within the page, a later byte for an address replacing an earlier one; programming only clears bits; BUSY for tPP |
| 20h, 52h, D8h Sector, 32 KB and 64 KB Block Erase | with WEL: the aligned block reads FFh; BUSY for tSE, tBE1, tBE2 |
| C7h, 60h Chip Erase | with WEL: everything FFh; BUSY for tCE |
| B9h Power-down, ABh Release Power-down | while powered down only ABh is taken |
| 66h then 99h Reset | WEL and BUSY clear, any operation ends |
| anything else | ignored |

Each is as the datasheet's 8.2 and 8.3 describe it under its own heading.
While BUSY every instruction but Read Status Register-1 is ignored (7.1),
and an erase or program ends with WEL cleared. The model keeps BUSY for the
datasheet's typical times (9.6): tPP 0.4 ms, tSE 45 ms, tBE1 120 ms, tBE2
150 ms, tCE 5 s, on the simulation clock, which ticks every millisecond.

The part has no BUSY pin: a host polls Status Register-1. The engine
therefore sets BUSY in the status it serves from the release of every
Page Program and erase window until the model has taken the window, so a
poll right after the window never sees the part idle before the model knows
of the command (`mclw_command_set_busy_status`). For a window the model
then ignores, for want of WEL say, that BUSY lasts only as long as the
worker takes to deliver it.

## Not modelled

The engine serves the memory read from address bits 15..0: bits 23..16
reach the model but select nothing, so a read above 64 KiB returns what the
same address in the first 64 KiB holds. A read returns at most 256 bytes;
past that the line holds its last bit. Fast Read (0Bh) and the dual and quad
reads, JEDEC ID (9Fh), Manufacturer/Device ID (90h) and the other ID and
SFDP reads, which all answer data where the image has no framing for it,
answer status bytes. Status Registers 2 and 3 and their writes, the block
protect and lock bits, the security registers, erase and program suspend,
and QPI are not modelled.

## pico-examples

`spi/spi_flash` (PL022 at 1 MHz) and `pio/spi/spi_flash` (PIO at 1 MHz on a
125 MHz RP2040, 1.2 MHz on a 150 MHz RP2350) erase sector 0, read page 0,
program it with 0..255, read it, erase and read again, printing each page.
Both run well under the memory read's 2 MHz.

## Sources

Winbond W25Q16JV datasheet, Revision D (August 12, 2016), in
`.dev/resources/datasheets/w25q16jv_spi_revd_08122016.pdf`.
