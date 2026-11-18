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
| `hello` | `version`, `i2c` `[{bus, sda, scl}]`, `uart` `[{port, tx, rx}]` | `version` |
| `tick` | | |
| `i2c` | `bus`, `ev`: `start` (`addr`, `read`, `repeated`) | `ack` |
| | `ev`: `write` (`data`) | `ack` |
| | `ev`: `read` | `data` |
| | `ev`: `stop` | |
| `gpio` | `pin`, `level` (0, 1, or `"z"` for not driven) | |

`hello` maps the emulator's buses to DUT pins, since models are attached by
the DUT pins they are wired to (`attach("mcp9808", "t", i2c=(4, 5, 0x18))`).
A UART's `tx` and `rx` are the DUT's own pins. `gpio` reports the level the
DUT drives on a pin; inputs of the harness read it.

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

A model makes an interrupt pulse as write, delay, write. The delay does not
wait: it moves the later actions of the same request on by its length, so
the pulse reaches the DUT with its width.

## Not yet

SPI (a synchronous `spi` request per frame, MOSI word in and MISO word
back, with chip selects as `gpio` requests), UART toward the harness, and
the single-wire parts (timestamped `gpio` edges on watched pins).
