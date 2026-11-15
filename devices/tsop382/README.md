<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# tsop382

An active-low IR receiver module in the path from the DUT's IR LED to its
receive pin: `tsop38238`, a Vishay TSOP38238 (TSOP382 series, 38 kHz). The
`wire` binding's `envelope` engine drives the receiver's output low while
the LED pin carries a carrier and times every burst and gap; the model
reads the NEC frames in them.

## Datasheets

Vishay TSOP382.., TSOP384.. document 82491 Rev. 2.1 (`TSOP382_vishay_82491.pdf`):
"Suitable data format" and the output pulse width (tpi - 5/f0 < tpo <
tpi + 5/f0). Vishay "Data Formats for IR Remote Control", document 80071
Rev. 2.3 (`vishay_80071_ir_data_formats.pdf`), "The NEC code" and figure 5.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `ir` | `wire` (`envelope`) | yes | `(led, out)`: the pin driving the IR LED, and the receiver's OUT |

```python
import metalclaw
rx = metalclaw.attach("tsop38238", "rx", ir=(14, 15))
rx.frames, rx.address, rx.command, rx.last, rx.repeats, rx.errors, rx.bursts
```

## Properties

All read-only.

| name | type | meaning |
| --- | --- | --- |
| `bursts` | u32 | carrier bursts seen |
| `frames` | u32 | NEC frames read |
| `repeats` | u32 | NEC repeat codes |
| `errors` | u32 | bursts or gaps that fit no frame |
| `last` | u32 | the last frame's 32 bits, its first bit in bit 0 |
| `address`, `command` | u32 | its first and third bytes |

## Behaviour

- **Output**: low from the first carrier edge until no edge has come for
  52 µs (two periods at 38 kHz), then high; the burst comes out about 33 µs
  longer and the gap shorter, inside the datasheet's five carrier periods.
  The engine times each to 2-3 µs.
- **NEC** (figure 5): a 9 ms leader burst, 4.5 ms, then 32 bits LSB first,
  each a 562.5 µs burst followed by the next 1.125 ms (0) or 2.25 ms (1)
  after its start, closed by a burst; or a leader, 2.25 ms and a burst for
  a repeat. The model accepts each length within a window wide enough for
  the receiver's stretch.

## Not modelled

- The carrier frequency: any edges closer than the gap count as carrier,
  where a real TSOP38238 passes only a band around 38 kHz.
- The AGC and its limits on burst length and duty cycle; the receiver's
  delay.

## Timing on the harness

The engine is one state machine at 1 MHz (14 instruction words, no DMA): it
sees the 6.5 µs carrier pulses pico-examples' NEC transmitter sends
(38.222 kHz, 25 % duty).
