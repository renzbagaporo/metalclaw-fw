<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# spi_page

The other end of an SPI link that moves 256-byte pages a frame at a time.
By default a frame is one byte per chip-select window, MSB first, mode 0:
how two ARM PL022s (the RP2040's and RP2350's SPI block) talk in mode 0,
since a PL022 raises its chip select between every two frames in Motorola
SPI with SPH 0. pico-examples' `spi_master` and `spi_slave` are each
other's part; these models stand in for either. Any SPI mode, 8- or 16-bit
frames, LSB first and windows of any length are a property away, for SPI
blocks configured otherwise. Two models:

- `spi_page_target`, for a DUT that is the controller: every frame shifts
  out the next frame of its `miso` page and keeps the frame the DUT sent at
  the same place in `mosi`, however many frames a window holds. A page is
  256 bytes, so byte k of the DUT's frames answers byte k mod 256. If the
  count stands still part way through a page for 5 ms of simulation time
  (a transfer shorter than a page, or the DUT reset under one) both rings
  start again at byte 0, so the next transfer lines up; what they hold stays
  readable.
- `spi_page_controller`, for a DUT that is the target: clocks `length`
  bytes of its `mosi` page `pages` times, `interval` ms apart, the first one
  an interval after `pages` is set, at 1 MHz (the rate spi_master runs at),
  in windows of `window` frames, and keeps what comes back in `miso` as it
  comes.

A 16-bit frame is two bytes in the pages, its high byte first. `miso` is
optional on both: a half-duplex DUT has none, and then the target answers
nothing and the controller reads back FFh.

The pages default to what the partner program sends: `spi_slave`'s
`out_buf[i] = ~i` for the target, `spi_master`'s `out_buf[i] = i` for the
controller. The engine is the port's SPI peer (`hal/spi_peer.h`): on rp2
one PIO state machine and two DMA channels, no CPU in the byte path. One of
each model can be attached at a time.

## Bindings

| model | `sck` | `mosi` | `miso` | `cs` |
| --- | --- | --- | --- | --- |
| `spi_page_target` | `gpio_in` | `gpio_in` | `gpio_out` | `gpio_in` |
| `spi_page_controller` | `gpio_out` | `gpio_out` | `gpio_in` | `gpio_out` |

`sck`, `mosi` and `cs` are required; `miso` is optional (a half-duplex DUT has none).

```python
import metalclaw
peer = metalclaw.attach("spi_page_target", "peer", sck=18, mosi=19, miso=16, cs=17)
peer.bytes                                  # windows the DUT has clocked
peer.read_region("mosi", 0, 256)            # what it sent
master = metalclaw.attach("spi_page_controller", "master", sck=18, mosi=16, miso=19, cs=17)
master.pages = 3                            # three pages, 500 ms apart
master.read_region("miso", 0, 256)          # what the last one brought back
```

## Properties

| model | name | type | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| target | `bytes` | u32 | bytes | read only | bytes of frames clocked since attach |
| both | `mode` | u32 | | 0 .. 3 | the SPI mode, CPOL in bit 1 and CPHA in bit 0; default 0 |
| both | `bits` | u32 | | 8 or 16 | bits a frame; default 8 |
| both | `lsb_first` | bool | | | bit order; default MSB first |
| controller | `length` | u32 | bytes | 1 .. 256 | of the page each transfer clocks, in whole frames; default 256 |
| controller | `window` | u32 | | 1 .. 256 | frames under one chip select; default 1 |
| controller | `pages` | u32 | | 0 .. 100 000 | pages left to clock |
| controller | `interval` | u32 | ms | 1 .. 60 000 | before each page; default 500 |
| controller | `clocked` | u32 | | read only | pages clocked since attach |

## Regions

| model | name | size | access | meaning |
| --- | --- | --- | --- | --- |
| target | `miso` | 256 | rw | the page it answers; a write is served from the next window, at byte 0 |
| target | `mosi` | 256 | ro | what the DUT sent, by position in the page |
| controller | `mosi` | 256 | rw | the page it sends; refused while one is in flight |
| controller | `miso` | 256 | ro | what the DUT answered, frame by frame as the transfer runs |

`mode` sets SCK's idle level (CPOL) and which edge each bit is sampled on
(CPHA 0: data out before the first edge of each bit and sampled on it;
CPHA 1: out on the first edge, sampled on the second), as the DUT's SPI
block is configured (some SPI targets only do CPHA 1). A new mode, frame
size or bit order on the target starts its rings again at byte 0, as a new
`miso` page does; the controller takes them, `length` and `window` only
between pages (`EBUSY` while one is in flight). A window ends on a frame
boundary: a target whose chip select rises part way through a frame takes
the rest from the next window.

`reset(True)` restores the default pages and format, `length` and
`window`; any `reset()`
restarts the target's rings and clears the controller's `pages` and
`clocked`. There are no faults.
