<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# drv2605

TI's DRV2605L haptic driver on I2C, as the model `drv2605l`: Table 3's
registers 00h-22h as a register map the harness serves from hardware, with
the waveform sequencer played on GO (data sheet SLOS854D, March 2018,
`DRV2605L.pdf`). Zephyr drives it as `ti,drv2605`.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, 0x5a)` |

```python
import metalclaw
hap = metalclaw.attach("drv2605l", "haptic", i2c=(4, 5, 0x5a))
hap.effect_ms = 100   # how long each ROM waveform plays
hap.played, hap.last, hap.playing
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `effect_ms` | u32 | rw | ms | each waveform's length, 100 at attach (the ROM library's lengths are in TI's effects list, not this data sheet) |
| `played` | u32 | ro | | waveforms played since attach |
| `last` | u32 | ro | | the last waveform identifier played |
| `playing` | bool | ro | | the sequencer is running |

## Behaviour

- Table 3's defaults: STATUS E0h (DEVICE_ID 7, the DRV2605L), MODE 40h
  (standby), and so on to 22h. STATUS is read-only; the rest take writes.
- DEV_RESET restores the defaults and clears itself (Table 5).
- GO set starts the sequencer at 04h (Table 8): a waveform identifier plays
  for `effect_ms`, a WAIT entry idles 10 ms per count, and a zero or the
  eighth slot ends it, when GO clears. Clearing GO cancels playback
  (Table 9). In diagnostics and auto-calibration modes GO clears at once,
  passing (DIAG_RESULT 0).
- `reset()` is a power cycle.

## Not modelled

The IN/TRIG and EN pins and the external-trigger modes, real-time
playback and the PWM, analog and audio-to-vibe inputs, the actuator
output, STANDBY's effect on playback, the OTP.
