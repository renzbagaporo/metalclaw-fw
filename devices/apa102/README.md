<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# apa102

A strip of APA102C LEDs on their clock and data inputs: `apa102`. The
harness samples data at every rising clock edge with the `wire` binding's
`clocked_in` engine, and the model parses the frames the datasheet
describes.

## Datasheet

iPixel LED / Shiji Lighting APA102C (`APA102C_ipixel.pdf`, five pages), page
3 "cascading data structure" and page 4 "PWM input and output signals
Relations".

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `spi` | `wire` (`clocked_in`) | yes | `(ci, di)`: the clock and data inputs |

```python
import metalclaw
strip = metalclaw.attach("apa102", "strip", spi=(6, 7))
strip.count = 150
strip.read_region("leds", 0, 600)   # per LED: 111 + brightness, blue, green, red
strip.frames, strip.leds_set, strip.end_frames
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `count` | u32 rw | 1 .. 256 | LEDs in the strip; 1 at attach |
| `frames` | u32 ro | | start frames (32 zeros) seen |
| `leds_set` | u32 ro | | LED frames taken |
| `end_frames` | u32 ro | | end frames (32 ones) after the last LED |

## Regions

| name | size | access | content |
| --- | --- | --- | --- |
| `leds` | 1024 | ro | each LED's last frame: `111` and the 5-bit global brightness, then blue, green, red |

## Behaviour

- SDI is taken at each rising edge of CKI, MSB first (p. 4).
- A start frame is 32 zeros; each LED frame is 32 bits, `111`, the global
  brightness, blue, green, red; an end frame 32 ones (p. 3). An LED frame
  starts with ones, so at most 29 zeros run inside the data: 32 zeros are
  always a start frame, and the model aligns on the first 1 after them, so
  it can join a stream anywhere.
- After a start frame the first LED takes the first frame, the second the
  next, and so on; frames beyond `count` pass the strip, and a frame of 32
  ones there counts as an end frame. An LED shows its frame once it has
  all 32 bits.

## Not modelled

- The half-clock each LED adds to the clock it passes on (not in the
  datasheet), which on a real strip needs extra clocks at the end.
- CKO and SDO, VEN self-test, POLAR and CSEL.

## Timing on the harness

The datasheet gives no clock limit. The engine is one state machine at the
system clock running three instructions a bit (wait for the clock low,
wait for it high, sample): with the input synchronizer's two cycles, the
clock's high and low halves each need a few harness cycles, tens of MHz.
pico-examples' `pio_apa102` clocks 5 MHz. A DMA channel streams the words
into a 2 KiB ring of 512, about 3 ms of a continuous 5 MHz stream.
