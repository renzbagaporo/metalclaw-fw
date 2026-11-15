<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# dht22

An Aosong DHT22 (AM2302) humidity and temperature sensor on its single
data line: `dht22`. The harness's `wire` binding runs the `responder`
engine: it waits for the host's start signal and answers with the 40-bit
waveform the model builds from its properties.

## Datasheet

Aosong DHT22 (also named AM2302) data sheet, Thomas Liu
(`DHT22_AM2302_aosong.pdf`), section 6(2) and its step 1 to 3 diagrams;
the AM2302 data sheet (`AM2302_aosong.pdf`) for the data format's example
and negative temperatures.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `data` | `wire` (`responder`) | yes | the DATA pin, open drain |

The bus idles high through its pull-up; the sensor only pulls it low. On a
bench without line pull-ups the DUT pad's pull-down, which `gpio_init`
leaves on, holds an undriven line near half the supply against the
harness's 50-80 kΩ pull-up, so the part works only where the bench pulls its
lines up (the MetalClaw Pico RP2's 5.1 kΩ).

```python
import metalclaw
s = metalclaw.attach("dht22", "s", data=24)
s.humidity = 45600       # m%RH
s.temperature = 23400    # mC
s.responses
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `humidity` | u32 rw | m%RH | 0 .. 100 000 | 50 000 at attach |
| `temperature` | i32 rw | mC | -40 000 .. 80 000 | the operating range (section 3); 25 000 at attach |
| `responses` | u32 ro | | | start signals answered |

## Behaviour

- **Start**: a host low of 1 ms or longer, then its release (step 1).
- **Response**: 30 µs after the release (the datasheet's 20-40 µs) the
  sensor pulls the bus low for 80 µs, releases it for 80 µs, then sends 40
  bits, each 50 µs low and then released 27 µs for a 0 (26-28 µs) or 70 µs
  for a 1, MSB first, and ends with 50 µs low (steps 2 and 3).
- **Data**: humidity and temperature in tenths, rounded, 16 bits each, the
  temperature's top bit its sign (AM2302 "When highest bit of temperature
  is 1"), then the low byte of the four bytes' sum. Every start gets the
  current values; a property set while a response plays applies from the
  next.

## Not modelled

- The measuring time and the two-second collecting period (section 7): any
  start is answered.
- Accuracy, resolution beyond tenths, hysteresis.

## Timing on the harness

The engine is one state machine at the system clock (15 instruction words)
playing the waveform from a DMA channel; the pulses are exact to a harness
cycle. pico-examples' `dht` reads the line with a `sleep_us(1)` loop and
calls a bit 1 when its high level lasted more than 16 passes: right for
these widths only when a pass takes about 2 µs. With 1 µs passes every 0
(27 µs) reads as 1 and the program prints "Bad data"; the host tests show
both (`test/unit/test_wire_models.py`).
