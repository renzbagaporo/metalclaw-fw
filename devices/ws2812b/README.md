<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# ws2812b

A strip of Worldsemi WS2812B LEDs on their single-wire input: `ws2812b`.
The harness decodes the line with the `wire` binding's `pulse_bits_in`
engine, checking each pulse against the datasheet's timing, and the model
keeps what each LED latched at the last reset.

## Datasheet

Worldsemi WS2812B, "Intelligent control LED integrated light source"
(`WS2812B_worldsemi.pdf`, the five-page sheet), sections "Data transfer
time", "Sequence chart", "Data transmission method" and "Composition of
24bit data".

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `din` | `wire` (`pulse_bits_in`) | yes | the strip's DIN, one pin |

```python
import metalclaw
strip = metalclaw.attach("ws2812b", "strip", din=6)
strip.count = 150
strip.read_region("leds", 0, 450)   # G, R, B of each LED, as sent
strip.frames, strip.bad_frames, strip.length
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `count` | u32 rw | 1 .. 256 | LEDs in the strip; 1 at attach |
| `channels` | u32 rw | 3 .. 4 | bytes each LED takes: 3 for the WS2812B; 4 for a strip of RGBW parts with the same timing |
| `frames` | u32 ro | | frames latched |
| `bad_frames` | u32 ro | | frames with a pulse outside the datasheet's windows, latched by no LED |
| `length` | u32 ro | bytes | the last good frame's length, bytes beyond the strip included |

## Regions

| name | size | access | content |
| --- | --- | --- | --- |
| `leds` | 1024 | ro | each LED's bytes in the order it received them: green, red, blue (and a fourth for `channels` 4) |

## Behaviour

- **Bits**: a high pulse of T0H, 0.4 µs ± 150 ns, is a 0 and one of T1H,
  0.8 µs ± 150 ns, a 1; MSB first, green, red, blue. The engine checks the
  pulse at 250 ns (still high), reads the bit at 600 ns, between the widest
  0 and the narrowest 1, and checks a 1 at 950 ns (low again). A pulse
  outside those windows spoils the frame. Pulses are timed to two harness
  cycles or so (16 ns at 125 MHz).
- **Reset**: a low of RES, above 50 µs, ends the frame. Each LED takes the
  first `channels` bytes that reach it and passes the rest on ("Data
  transmission method"); at the reset every LED that received all its
  bytes shows them. The model keeps the bytes as sent; gamma and the
  PWM's output are not modelled.

## Not modelled

- The low times T0L and T1L (0.85 and 0.45 µs ± 150 ns) are not checked;
  any low shorter than RES continues the frame.
- DOUT: no LED re-sends anything.
- The datasheet's supply-dependent input levels (VIH 0.7 VDD).

## Timing on the harness

The engine is one state machine running at the system clock (125 MHz on a
Pico, 150 MHz on the MetalClaw Pico RP2): 26 instruction words, a DMA
channel and a 2 KiB ring of 1024 entries, about 10 ms of a continuous
800 kHz stream; the model's poll empties it every millisecond.
