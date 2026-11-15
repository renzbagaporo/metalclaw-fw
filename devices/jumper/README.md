<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# jumper

A jumper wire between two of the DUT's pins, made by the harness pins that
reach them. Two models:

- `jumper` carries a push-pull signal one way: the harness drives `sink`
  at the level the DUT drives on `source`. On rp2 that is one PIO instruction,
  `mov pins, pins`, run every cycle: `sink` follows two synchronizer cycles
  and one instruction behind, about 25 ns at 125 MHz, with one cycle of
  jitter. Fast enough for a 921 600 baud UART, a 10 Mbit/s Manchester line
  or an SPI clock of a few tens of MHz.
- `jumper_open_drain` joins two open-drain lines both ways, as a wire
  between two I2C buses does for SDA or SCL: a side held low by what is on
  it is driven low on the other side, and both sides have the harness's
  pull-ups. The harness cannot tell its own low from one a part drives, so
  it remembers which side it drives: after it lets a side go it waits 2 µs
  (on rp2 256 cycles at 125 MHz) for that side to rise before it looks at
  it again, and a side still low then is held by its own parts. That wait
  outlasts the rise of a line held up only by pad pull-ups and stays under
  the 4.0 µs minimum SCL high time of a 100 kHz bus (UM10204 table 10).
  While a target stretches SCL past the moment the controller lets it go,
  the controller sees SCL high for that wait first.

The link lives in the port's hardware from attach to detach; on rp2 each is
one PIO state machine with no CPU or DMA, on PIO0, then PIO2, then PIO1, and
both pins must be in that block's 32-pin window.

## Bindings

| model | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| `jumper` | `source` | `gpio_in` | yes | the pin the DUT drives |
| `jumper` | `sink` | `gpio_out` | yes | the pin the harness drives after it |
| `jumper_open_drain` | `a` | `gpio_in` | yes | one side |
| `jumper_open_drain` | `b` | `gpio_in` | yes | the other side |

```python
import metalclaw
tx_to_rx = metalclaw.attach("jumper", "tx_to_rx", source=4, sink=3)
sda = metalclaw.attach("jumper_open_drain", "sda", a=4, b=6)
scl = metalclaw.attach("jumper_open_drain", "scl", a=5, b=7)
```

## Properties

| name | type | unit | range | meaning |
| --- | --- | --- | --- | --- |
| `linked` | bool | | read only | whether the port holds the link |

There are no regions and no faults. Attach fails with `OSError` (`ENOSPC`)
when the port has no state machine or instruction memory left for it.
