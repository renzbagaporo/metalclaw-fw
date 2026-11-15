<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# hub75

A 128 × 64 HUB75 LED panel scanned 1/32: `hub75`. The `wire` binding's
`latched_in` engine looks at the bus one latch at a time: the data clocked
in before a latch pulse, the row address at the latch, and how long the
output enable is asserted until the next latch. The model keeps, for every
row, every eighth column of each distinct latch it has seen.

## Datasheets

HUB75 has no datasheet. The panel's interface is the program's own
description (pico-examples `pio/hub75/Readme.md` and `hub75.pio`); its
column drivers are 16-bit constant-current sinks of the TI TLC5926 kind
(`TLC5926_slvs677c.pdf`, SLVS677C, pin functions): data shifts in on CLK's
rising edge, LE high passes it to the outputs' latches, OE low turns the
outputs on.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `clock` | `wire` (`latched_in`) | yes | `(clk, lat, oe)` |
| 1 | `upper` | `wire` (`pins`) | yes | `(r0, g0, b0)`: rows 0-31 |
| 2 | `lower` | `wire` (`pins`) | yes | `(r1, g1, b1)`: rows 32-63 |
| 3 | `rows` | `wire` (`pins`) | yes | `(a, b, c, d)` |
| 4 | `e` | `wire` (`pins`) | yes | `(e,)` |

Every pin must be within the 32 GPIOs one PIO block reaches.

```python
import metalclaw
panel = metalclaw.attach("hub75", "panel", clock=(16, 27, 26), upper=(3, 2, 6),
                         lower=(0, 11, 10), rows=(14, 8, 19, 18), e=(22,))
panel.slots                                # (row, enable time) pairs seen, of 256
panel.read_region("thumbnail", 0, 3072)    # 64 rows x 16 columns x RGB
```

## Properties

All read-only.

| name | type | meaning |
| --- | --- | --- |
| `latches` | u32 | latches recorded |
| `rows_seen` | u32 | a bit per row address with a latch |
| `slots` | u32 | distinct (row, enable time) pairs held, of 32 × 8 |
| `unmatched` | u32 | latches that fit no slot of their row, or enabled for no time |
| `short` | u32 | looks with fewer than 128 clocks before the latch, or a clock missing |

## Regions

| name | size | access | content |
| --- | --- | --- | --- |
| `planes` | 5120 | ro | per row address and slot: the enabled time in ns (4 bytes, little endian), then a byte per thumbnail column: R0, G0, B0, R1, G1, B1 in bits 0..5 |
| `thumbnail` | 3072 | ro | per panel row and column 8c: red, green, blue, each 255 × the share of the row's enabled time its bit was latched on |

## Behaviour

- A latch takes the last 128 bits clocked into each data line: the first
  of them is column 0, at the far end of the chain. The row address is A..E
  at the latch; R0, G0, B0 light that row, R1, G1, B1 the row 32 below.
- A row's latches are told apart by their enabled time: a new latch whose
  time is within an eighth of a slot's replaces it, otherwise it takes a
  free slot (eight per row). A pixel's colour is the time its bit was on
  over the row's enabled time, which for binary-weighted planes such as
  hub75.c's (100 × 2^bit cycles) is the 8-bit value.
- The model re-arms a look as soon as it has read one, about once a
  millisecond, so a static image fills in within seconds.

## Not modelled

- Brightness over time, persistence, and timing within a scan: only the
  latched data and enable times count.
- Columns other than every eighth.

## Timing on the harness

Two state machines at the system clock (15 instruction words each) and
a DMA channel into a 2 KiB block. The data machine polls in four-cycle
passes, reading every pin at once when it sees the clock high, so a sample
is at most 27 ns (150 MHz) after the edge; hub75.pio changes data seven DUT
cycles (47-56 ns) after its rising edge, and its latch pulse lasts eight
(53-64 ns). The enable time is counted in five-cycle passes, 33 ns at
150 MHz, against 800 ns for the shortest plane. A look holds up to 512
clocks between latches.
