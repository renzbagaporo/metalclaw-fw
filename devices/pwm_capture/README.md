<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# pwm_capture

A PWM output of the DUT measured by the harness: `pwm_capture`. The
`wire` binding's `edges_in` engine times every high and low on the pin;
the model keeps the last whole cycle, rise to rise, as its period and its
high time. For a servo or ESC signal, a motor's or LED's PWM: what the DUT
really drives on the pin, not what it says it set.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `pin` | `wire` (`edges_in`) | yes | `(pin,)`: the DUT's PWM output |

```python
import metalclaw
servo = metalclaw.attach("pwm_capture", "servo", pin=(33,))
servo.period_ns, servo.pulse_ns, servo.duty_ppm   # 20000000 1500000 75000
servo.still, servo.level, servo.periods, servo.missed
servo.idle_ms = 100
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `period_ns` | u32 | ro | ns | the last whole cycle's length, rise to rise; 0 before one |
| `pulse_ns` | u32 | ro | ns | its high time; while `still`, `period_ns` or 0 by the level |
| `duty_ppm` | u32 | ro | ppm | `pulse_ns` / `period_ns`; while `still`, 1000000 or 0 by the level |
| `still` | bool | ro | | no edge for two of the last cycles (or `idle_ms` before one), and the poll's 2 ms: a duty of 0 % or 100 %, or a stopped output |
| `level` | i32 | ro | | the pin now: 0, 1, or -1 before the port can tell |
| `periods` | u32 | ro | | whole cycles timed since attach or `reset()` |
| `missed` | u32 | ro | | times the engine lost entries; a cycle across one is not timed |
| `idle_ms` | u32 | rw | ms | 1 .. 60000; how long a line with no cycle yet must hold to be `still`; 100 at attach |

## Behaviour

- A cycle is a high and the low after it, both timed and nothing lost
  between: a new duty shows in `pulse_ns` with the first whole cycle at
  it, within two periods of the change.
- A level of 2.1 s or longer, and the one the pin stood at on attach, end
  no cycle.
- `reset()` clears the cycle, the counts and `still`'s history;
  `reset(True)` also sets `idle_ms` back to 100.

## Not modelled

- The load: the pin is only an input.

## Timing on the harness

As [`logic_capture`](../logic_capture/README.md)'s: the `edges_in` engine
times each level to two or three harness cycles (16-24 ns at 125 MHz),
and keeps up with a PWM of a few kHz (four channels at 1 kHz together);
past about 3 kHz on one pin its FIFO can fill between polls and `missed`
counts.
