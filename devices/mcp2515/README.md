<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# mcp2515

Microchip MCP2515 stand-alone CAN controller on SPI, as the model `mcp2515`
(datasheet DS20001801J). The DUT drives it with the part's SPI instruction
set: it configures bit timing, masks and filters, changes mode, loads and
sends transmit buffers, and takes received messages on the INT pin.

The model is a command image (`metalclaw/command.h`). READ (03h) is the
image's short register read: the instruction, one address byte, and the
registers from there, served from hardware. READ RX BUFFER (90h, 92h, 94h,
96h), READ STATUS (A0h) and RX STATUS (B0h) are its immediate responses,
which the model keeps in step with the registers. WRITE, LOAD TX BUFFER,
RTS, BIT MODIFY and RESET reach the model after chip select rises.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `spi` | `spi_target` | yes | an SPI target, from the tuple `(sck, mosi, miso, cs)` |
| 1 | `int` | `gpio_out` | no | INT, low while an enabled interrupt flag is set (7.0) |

SPI modes 0,0 and 1,1, MSB first (12.0). The immediate responses lower the
clock the harness serves the part at (`mclw_hal_spi_target_max_clock_hz`)
to 250 kHz on an RP2040 harness, which answered them late at 500 kHz, and
to 1 MHz on an RP2350 one, exact to 2 MHz and late at 4 MHz; the part runs
to 10 MHz.

WRITE and BIT MODIFY take effect at chip select's rise, as on the part:
the harness applies them to the registers it serves at the window's
release, a few microseconds before the model has taken the window, so a
READ right after reads the new value (CANSTAT's OPMOD after a mode change
included).

```python
import metalclaw
can = metalclaw.attach("mcp2515", "can", spi=(18, 19, 16, 17), int=15)
can.mode, can.tec, can.sent, can.received
bytes(can.read_region("registers", 0, 128))
```

## Properties and regions

| name | type | access | meaning |
| --- | --- | --- | --- |
| `mode` | u32 | ro | CANSTAT's OPMOD: 0 Normal, 1 Sleep, 2 Loopback, 3 Listen-only, 4 Configuration |
| `tec` | u32 | ro | the transmit-error count |
| `sent`, `received` | u32 | ro | messages sent and received since attach or reset |
| `overflows` | u32 | ro | messages lost to a full receive buffer |
| `ignored` | u32 | ro | instructions the model does not know |
| `spi_mode` | u32 | rw | 0 or 3 |

| region | size | meaning |
| --- | --- | --- |
| `registers` | 128 | the register map, 00h .. 7Fh, ro |

## Behaviour

- RESET and attach leave every register zero but CANCTRL (87h) and CANSTAT
  (80h): Configuration mode (table 11-2). CANCTRL and CANSTAT read alike at
  every xFh and xEh.
- REQOP takes effect at once: CANSTAT's OPMOD follows it. ABAT ends every
  pending transmission with ABTF. CNF1..CNF3, the filters and the masks
  take writes only in Configuration mode; the status registers, the error
  counts and the received messages take none. BIT MODIFY works on the
  registers table 11-1 marks, and on any other as a full write.
- A transmit request (RTS, or TXREQ written) is served at the next
  millisecond tick, one buffer a tick, highest TXP first and the higher
  buffer among equals (3.2). In Loopback mode the controller receives its
  own message: TXREQ clears and TXnIF sets. In Normal mode no node
  acknowledges it: TXERR sets and TEC climbs by 8 a tick to 128,
  error-passive, with EWARN/TXWAR at 96 and TXEP at 128 and ERRIF as each
  first sets; the message stays pending, or is aborted under OSM.
  Listen-only, Sleep and Configuration send nothing.
- Reception (4.2, 4.5): RXB0 takes what RXF0 or RXF1 passes under RXM0, or
  anything with RXM 11, rolling over into RXB1 with BUKT; RXB1 takes what
  RXF2..RXF5 pass under RXM1. A filter matches its EXIDE type in every bit
  the mask sets. The message, RXRTR (SRR for a standard remote frame) and
  FILHIT are stored and RXnIF sets; a full buffer loses it, with RXnOVR and
  ERRIF. READ RX BUFFER clears the buffer's RXnIF when chip select rises.
- CANSTAT's ICOD reports the highest enabled pending interrupt.

## Not modelled

The bit timing (a message takes a tick whatever CNF says), a real CAN bus
and other nodes, the receive-error count, bus-off, Sleep's wake-up, the mask
and filter bits a standard frame applies to its first two data bytes,
RXnBF and TXnRTS pins, CLKOUT and SOF, and the timing of a window the model
has not taken yet: a READ that follows a write within the worker's latency
reads the registers from before it.
