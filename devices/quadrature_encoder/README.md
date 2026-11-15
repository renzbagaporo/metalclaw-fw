<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# quadrature_encoder

An incremental quadrature encoder the harness turns: `quadrature_encoder`.
The `wire` binding's `steps_out` engine drives channels A and B through
the states the model queues, one each period, so every step comes exactly
1/rate after the last.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `ab` | `wire` (`steps_out`) | yes | `(a, b)`: consecutive pins, A first |

```python
import metalclaw
enc = metalclaw.attach("quadrature_encoder", "enc", ab=(10, 11))
enc.travel = 2000     # back and forth over 2000 steps; 0 turns one way for good
enc.rate = 1000       # steps a second, negative the other way, 0 stops
enc.position, enc.steps
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `rate` | i32 rw | steps/s | -100 000 .. 100 000 | 0 at attach: stopped |
| `travel` | u32 rw | steps | | turn back after this many steps; 0 never |
| `position` | i32 ro | steps | | where the shaft is, as queued, up to 128 steps ahead of the pins |
| `steps` | u32 ro | | | steps queued since attach |

## Behaviour

- A step is one quarter of a cycle, one of channel A or B changing. Forward,
  B leads A: A and B go 00, 01, 11, 10 read as (A, B) = (0, 0), (0, 1), (1,
  1), (1, 0), which pico-examples' `quadrature_encoder.pio` and
  `quadrature_encoder_substep.pio` both count up. Position 0 is both low,
  where the pins start.
- Steps are evenly spaced at 1/`rate`, from the harness's PIO clock. A new
  rate takes effect once the queue (up to 128 steps) has gone out; the
  motion carries on from where it was.

## Not modelled

- Contact bounce, phase error between the channels, an index pulse.

## Timing on the harness

The engine is one state machine at the system clock (3 instruction words),
16 states a FIFO word, fed from the model's poll every millisecond: up to
128 steps a millisecond. A state lasts at least four harness cycles.
