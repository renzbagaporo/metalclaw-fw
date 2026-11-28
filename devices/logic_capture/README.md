<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# logic_capture

One of the DUT's digital outputs watched by the harness: `logic_capture`.
The `wire` binding's `edges_in` engine times every high and low on the
pin; the model counts the edges and keeps the last level of each kind. For
a status line, a chip select, a pyro or relay channel: whether and how
often the DUT switched it, and for how long.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `pin` | `wire` (`edges_in`) | yes | `(pin,)`: the DUT output |

```python
import metalclaw
line = metalclaw.attach("logic_capture", "drogue", pin=(11,))
line.level, line.rising, line.falling, line.edges, line.last_edge_ms
line.high_ns, line.low_ns, line.missed
line.reset()          # the counts start again
```

## Properties

All read-only.

| name | type | unit | meaning |
| --- | --- | --- | --- |
| `level` | i32 | | the pin now: 0, 1, or -1 before the port can tell (sim, until the DUT reports the pin) |
| `rising`, `falling` | u32 | | edges of each kind since attach or `reset()` |
| `edges` | u32 | | both together |
| `last_edge_ms` | u32 | ms | the clock (`metalclaw.now_us()` / 1000) when the latest edge was read, to the poll (1 ms); 0 before the first |
| `high_ns`, `low_ns` | u32 | ns | the latest high and low, each as its closing edge ended it; 2147483647 for one of 2.1 s or longer, or the level the pin stood at on attach |
| `missed` | u32 | | times the engine lost entries (each one edge or more), see below |

## Behaviour

- An edge is counted when it happens, including the first after attach:
  a line that goes high once and stays high reads `rising` 1.
- `reset()` clears the counts and times; the level stays.

## Timing on the harness

The engine is one state machine at the system clock (18 instruction
words, no DMA) counting each level in passes of two cycles: a level is
timed to two or three harness cycles (16-24 ns at 125 MHz), and the levels
of a cycle add up to its period with no loss between them. Its entries
wait in the 8-entry joined RX FIFO for the model's poll, about once a
millisecond: a line that switches faster than about 3 kHz for long can
fill it, and `missed` then counts. On sim the engine is the DUT's own edge
times, to the ns.
