<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# ads7883

A Texas Instruments ADS7883 12-bit converter on SPI. The part has no
registers: every chip-select window clocks out one 16-bit frame carrying the
conversion result, and holds SDO low afterwards. The host sets the sample in
LSB; the DUT clocks the frame and reads it back.

The model is one file with no bus code. It fills a `struct mclw_stream`
with the two-byte frame and arms the SPI target engine with it; the engine
serves every window from DMA without calling the model. A new sample is
published into the stream's second buffer and swapped in, so a window never
sees half of each. The engine arms the new frame at once when idle, or at
the end of the next window when windows are arriving back to back.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `spi` | `spi_target` | yes | an SPI target, from the tuple `(sck, mosi, miso, cs)` |

The binding advertises what the datasheet says: SPI modes 0 and 3, mode 0 by
default, MSB first. The clock is the DUT's choice; the engine publishes the
ceiling it was qualified at in `stats("spi")["max_clock_hz"]`.

On fixture 1 that is SCK on GP18, MOSI on GP19, MISO on GP16 and chip select
on GP21:

```python
import metalclaw
adc = metalclaw.attach("ads7883", "adc", spi=(18, 19, 16, 21))
adc.sample = 0xABC
adc.spi_mode = 3
adc.stats("spi")
adc.reset()
adc.detach()
```

## Properties

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `sample` | u32 | rw | LSB | 0 .. 4095 | the conversion result the next window carries |
| `spi_mode` | u32 | rw | | 0 or 3 | the SPI mode the target listens in; other values are refused |

`sample` defaults to 0. There are no regions and no faults.

## The frame

```text
bit 15 14 | 13 .. 2 | 1 0
     0  0 | D11..D0 | 0 0
```

Two leading zeros, the result MSB first, two trailing zeros, then `0x00` for
as long as the DUT keeps clocking inside the window. `mclw_ads7883_frame()`
is the encoding.

## The DUT image

Zephyr v4.4.0 has no driver for this part, so `test/rig/dut/src/main.c` clocks
the frame itself through the SPI API on a `vnd,spi-device` node aliased `adc`
in `test/dut/rp2.overlay`: `spi0` on GP18/GP19/GP16 with a GPIO chip select
on GP21, because the PL022's own chip select drops whenever its transmit FIFO
drains and the frame is one chip-select window by definition. The overlay
sets `spi-max-frequency` to 15 MHz and mode 0.

The DUT command is `SPI adc [hz] [bytes] [count] [gap_us]`: `count` windows of
`bytes` bytes at `hz` (0 for the overlay's clock), `gap_us` apart. It reports
the first frame as hex and how many later windows differed from it. The image
knows nothing about the frame layout; `test/cases.py` decodes the sample and
the two zero fields out of the hex.

```sh
just build-dut ads7883
METALCLAW_HARNESS_SERIAL=<serial> METALCLAW_ARM_TOOLCHAIN_DIR=<dir> \
METALCLAW_DUT_DEVICE=ads7883 just test hardware
```

## Tests

`test/cases.py` attaches the model on the rig's SPI pins for this part
(`(18, 19, 16, 21)` on fixture 1, from the platform's `fixture` description),
resets the DUT and checks: five samples across the range read back exactly with both zero fields
clear; a new sample is in the very next window; a six-byte window is the frame
followed by zeros; ten windows move `transactions` by at least ten; at 1, 5,
10 and 15 MHz a thousand windows each all carry the same exact frame with no
stale windows and no DMA faults; a `reset()` puts the sample back to 0.
