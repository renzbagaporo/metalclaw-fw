<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# i2c_page

`i2c_page_controller`: the harness as the I2C controller of a DUT that is
the target, the I2C counterpart of [`spi_page_controller`](../spi_page/README.md).
Each page writes the `out` page to the DUT's address in `chunk`-byte write
transfers, then reads 256 bytes back into `in` in `chunk`-byte read
transfers; `pages` pages, `interval` ms apart, the first one an interval
after `pages` is set. Every transfer is START, the address, the bytes, STOP;
reads acknowledge every byte but each transfer's last. The DUT may stretch
the clock up to 25 ms. The engine is the port's I2C controller
(`hal/i2c_controller.h`): on rp2 the hardware I2C block behind the two pins,
fed from the worker, so the pins must be one block's SDA and SCL pair, and
no I2C target may hold that block at the same time. The host port has no
controller yet (the bridge has no harness-driven I2C), so attaching there
fails with `ENOTSUP`.

With `offset`, every write starts with the page offset of its first byte,
and every read is a write of that offset, a repeated START and the read:
how an EEPROM-style target takes an address (pico-examples'
`slave_mem_i2c`). Without it, the target keeps its own place.

A transfer the DUT ends with a NACK counts in `nacks`, its cause in
`last_error`, and the page goes on with its next chunk.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `sda` | `gpio_out` | yes | the data line |
| 1 | `scl` | `gpio_out` | yes | the clock line |

```python
import metalclaw
c = metalclaw.attach("i2c_page_controller", "ctl", sda=12, scl=13)
c.address = 0x42          # the DUT's 7-bit target address
c.pages = 3               # three pages, 500 ms apart
c.read_region("in", 0, 256), c.nacks, c.last_error
```

## Properties

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `address` | u32 | rw | | 0x08 .. 0x77 | the DUT's address; 0 until set, and `pages` needs it |
| `pages` | u32 | rw | | 0 .. 100 000 | pages left to run |
| `interval` | u32 | rw | ms | 1 .. 60 000 | before each page; default 500 |
| `rate` | u32 | rw | kHz | 100 or 400 | the clock rate; default 100 |
| `chunk` | u32 | rw | bytes | 1 .. 256 | bytes a transfer; default 16 |
| `offset` | bool | rw | | | each transfer starts with the page offset; default off |
| `clocked` | u32 | ro | | | pages completed |
| `transfers` | u32 | ro | | | transfers started |
| `nacks` | u32 | ro | | | transfers the DUT ended with a NACK |
| `last_error` | u32 | ro | | | the last failed transfer: 1 address NACK, 2 data NACK, 3 SDA or SCL low before a START, 4 SCL held over 25 ms; 0 none |

## Regions

| name | size | access | meaning |
| --- | --- | --- | --- |
| `out` | 256 | rw | the page written, by default `out[i] = i`; refused while a page runs |
| `in` | 256 | ro | what the last page read back |

`reset(True)` restores `out`; any `reset()` clears `pages` and the counters
(refused while a transfer is out). There are no faults.
