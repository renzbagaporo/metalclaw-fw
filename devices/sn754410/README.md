<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# sn754410

A Texas Instruments SN754410 quadruple half-H driver in the path from the
DUT to a motor: `sn754410`. Each input the DUT drives (1A, 2A, 1,2EN and,
for the second pair, 3A, 4A, 3,4EN) is a `wire` binding whose `edges_in`
engine times it; the model gives each input's duty and period, each
output's state by the function table, and each pair's drive. It stops at
the outputs: the motor and its load stay in the test.

## Datasheets

Texas Instruments, SN754410 Quadruple Half-H Driver, SLRS007C (revised
January 2015) (`sn754410_ti_slrs007.pdf`): 9.4 Device Functional Modes,
Table 1 (function table); 9.3.2 TTL Compatible Inputs (an open input
approaches VCC1); 7.5 Switching Characteristics.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `a1` | `wire` (`edges_in`) | yes | `(pin,)`: 1A |
| 1 | `a2` | `wire` (`edges_in`) | no | `(pin,)`: 2A |
| 2 | `en12` | `wire` (`edges_in`) | no | `(pin,)`: 1,2EN |
| 3 | `a3` | `wire` (`edges_in`) | no | `(pin,)`: 3A |
| 4 | `a4` | `wire` (`edges_in`) | no | `(pin,)`: 4A |
| 5 | `en34` | `wire` (`edges_in`) | no | `(pin,)`: 3,4EN |

An input not wired reads high (9.3.2): leave out `en12` for an enable tied
to VCC1.

```python
import metalclaw
motor = metalclaw.attach("sn754410", "motor", a1=(8,), a2=(9,), en12=(10,))
motor.drive_ppm[0]        # 1Y over 2Y, as a share of VCC2: 400000 forward at 40 %
motor.duty_ppm, motor.period_ns, motor.outputs, motor.levels
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `levels` | i32[6] | ro | | each input now (1A, 2A, 1,2EN, 3A, 4A, 3,4EN): 0, 1, or -1 before the port can tell; 1 for one not wired |
| `duty_ppm` | u32[6] | ro | ppm | each input's duty over its last whole cycle, or 0 / 1000000 by its level while it holds (as `pwm_capture`'s); 1000000 for one not wired |
| `period_ns` | u32[6] | ro | ns | each input's last cycle, 0 while it holds or before one |
| `outputs` | i32[4] | ro | | 1Y-4Y now by Table 1: 1 high, 0 low, 2 off (high impedance, EN low), -1 unknown |
| `drive_ppm` | i32[2] | ro | ppm | each pair's mean output difference as a share of VCC2, 1Y over 2Y and 3Y over 4Y: (d(A1) - d(A2)) x d(EN) |
| `missed` | u32 | ro | | times an input's engine lost entries |
| `idle_ms` | u32 | rw | ms | 1 .. 60000; as `pwm_capture`'s, for every input; 100 at attach |

## Behaviour

- **Function table** (Table 1): EN high, A high: Y high; EN high, A low: Y
  low; EN low: Y off.
- **Drive**: (d(1A) - d(2A)) x d(1,2EN), as the outputs average over a
  cycle. Exact while one of the three switches and the others hold
  (sign-magnitude: PWM on the enable with A steady, or on one A with the
  other held and the enable high) and for locked anti-phase (1A and 2A
  complementary at the same period); for two inputs switching at once
  otherwise, the product of their means.
- Both A equal with the enable high is a brake, drive 0; the enable low
  is a coast, both outputs off.

## Not modelled

- The switching delays (td1 400 ns, td2 800 ns, enable and disable 400-900
  ns, typical): the drive is a mean over whole cycles.
- VCC1, VCC2, output current, thermal shutdown (outputs off), the clamp
  diodes; the motor.

## Timing on the harness

One `edges_in` state machine per wired input (three for a pair), as
[`logic_capture`](../logic_capture/README.md)'s; each keeps up with a PWM
of a few kHz.
