<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# The host harness bridge

The host harness (`just build-host`, `core/ports/host`) serves its device
models to a DUT emulated elsewhere, a Renode machine, through this bridge.
The emulator owns the DUT and its clock; the harness answers the DUT's bus
events and drives its lines. Version 1.

## Transport

- A Unix stream socket. The harness listens at the path in
  `METALCLAW_BRIDGE`; the emulator connects, retrying until the harness is
  up. One connection per DUT; when it closes, the harness listens again.
- Newline-delimited JSON, one message per line, integers only.
- Lockstep: the emulator sends a request, the harness answers it with one
  reply, and the DUT's clock waits for the reply. Nothing on the harness
  side runs ahead of the DUT's time, so a run is the same at any host load.

## Time

Every request carries `t_ns`, the DUT's virtual time. Before handling a
request the harness moves its clock to `t_ns` (never back), walking long
steps a millisecond at a time so every model's `time_advanced` runs at the
tick it would have. The runtime's clock (`metalclaw.now_us()`) is therefore
the DUT's time.

The emulator also sends `{"op":"tick","t_ns":...}` every millisecond of DUT
time and when the DUT is idle, so the models' clock moves while the DUT is
quiet.

## Requests

| op | fields | reply |
| --- | --- | --- |
| `hello` | `version`, `i2c` `[{bus, sda, scl}]`, `uart` `[{port, tx, rx}]`, `spi` `[{bus, sck, mosi, miso}]` | `version` |
| `tick` | | |
| `route` | `i2c`, `uart`, `spi` as in `hello` | |
| `i2c` | `bus`, `ev`: `start` (`addr`, `read`, `repeated`) | `ack` |
| | `ev`: `write` (`data`) | `ack` |
| | `ev`: `read` | `data` |
| | `ev`: `stop` | |
| `uart` | `port`, `data` (a byte or a list of bytes the DUT sent) | |
| `spi` | `bus`, `mosi`, `bits` (8) | `miso` |
| `gpio` | `pin`, `level` (0, 1, or `"z"` for not driven) | |
| `spi_clocked` | `bus`, `mosi`, `miso` (the frame the DUT's slave shifted out, or null) | |

`route` reports where the DUT has routed its blocks, at the first tick
and whenever its routing changes; each block it names replaces the pins
`hello` gave. A bus request may also carry `pins`, the pins its block is
routed to then, which apply to it alone (and, for I2C, until the next
START). `hello` maps the emulator's buses to DUT pins, since models are attached by
the DUT pins they are wired to (`attach("mcp9808", "t", i2c=(4, 5, 0x18))`).
A UART's `tx` and `rx` are the DUT's own pins. `gpio` reports the level the
DUT drives on a pin; inputs of the harness read it, and a target's chip
select opens and closes its SPI window. An `spi` frame's MISO byte depends
only on the frames before it in the window, as on the wire.

An I2C `start` to an address no attached model answers is not acknowledged.
A `start` while a transaction is open is a repeated START and ends that
transaction; `stop` ends it.

## Actions

Any reply may carry `"actions"`: what the harness does to the DUT's lines,
each at its own `t_ns`, never earlier than the request's. The emulator
applies each at its time.

| op | fields |
| --- | --- |
| `gpio` | `pin`, `level` (0, 1, or `"z"` to release) |
| `uart` | `port`, `data` (one byte; the harness paces bytes at the baud rate) |
| `watch` | `pin`: from now on, report every level the DUT drives on it as a `gpio` request |
| `spi_clock` | `bus`, `mosi`: clock one frame into the DUT's SPI block, a slave, at `t_ns`; answered by an `spi_clocked` request |

A model makes an interrupt pulse as write, delay, write. The delay does not
wait: it moves the later actions of the same request on by its length, so
the pulse reaches the DUT with its width.

## Wires

A part whose timing is its protocol (an LED strip's data line, 1-Wire, a
DHT22's line) asks for its pins to be watched, times the DUT's edges as
they arrive, and drives the line with `gpio` actions at the times the part
would, as the rp2 port's PIO engines do (`core/ports/host/wire.c`).

A part that steps its own pins (a quadrature encoder) queues timed `gpio`
actions up to a few periods ahead; one that answers a carrier (an IR
receiver) drives its output from the carrier's first edge to the end of
the burst, which it sees at the next round, so up to a millisecond late.

## Not yet

The latched wire (a HUB75 panel).
