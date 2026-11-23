<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# mpl3115a2

An NXP MPL3115A2 pressure and altitude sensor on I2C, with its INT1 and INT2
lines. The host sets pressure, altitude and temperature; in active mode the
model takes a sample every acquisition step and puts it in the output
registers or, with the FIFO enabled, in the 32-sample FIFO, and raises the
interrupts the DUT enabled on the pin it routed them to.

The model fills one 256-byte `struct mclw_regmap`: the registers at their
own addresses, then the alias `F_STATUS` reads at 00h in FIFO mode, then the
FIFO's 160 bytes, then four zero bytes. A pointer map does the part's
aliasing: with `F_SETUP.F_MODE` non-zero, 00h reads `F_STATUS`, 01h (and
`F_DATA`, 0Eh) starts at the FIFO, so one burst from 01h drains it, and
02h..05h read zero. `on_write` handles `CTRL_REG1` (active, one-shot, reset)
and `F_SETUP`; the worker samples in `time_advanced` and drives the pins.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from the tuple `(sda, scl, address)` |
| 1 | `int1` | `gpio_out` | no | INT1 |
| 2 | `int2` | `gpio_out` | no | INT2 |

The address is 0x60. Both pins are driven push-pull at the level `IPOL1` or
`IPOL2` makes inactive from attach on, whatever `PP_OD` says, so a DUT
pull-up changes nothing.

```python
import metalclaw
alt = metalclaw.attach("mpl3115a2", "alt", i2c=(4, 5, 0x60), int1=16)
alt.altitude = 321500       # mm
alt.temperature = 21625     # mC
alt.pressure = 95000        # Pa
alt.reset()
alt.detach()
```

## Properties

| name | type | unit | range | meaning |
| --- | --- | --- | --- | --- |
| `pressure` | i32 | Pa | 20 000 .. 110 000 | what barometer mode (`CTRL_REG1.ALT` 0) reports |
| `altitude` | i32 | mm | -698 000 .. 11 775 000 | what altimeter mode (`ALT` 1) reports |
| `temperature` | i32 | mC | -40 000 .. 85 000 | the temperature both modes report |

The pressure range is the part's operating range (section 1); the altitude
range is roughly the altitudes of that range at the 101 326 Pa sea level of
9.1.3. Defaults: 101 326 Pa, 0 mm, 25 000 mC. Pressure and altitude are separate:
the model does not derive one from the other through `BAR_IN`. Altitude is
Q16.4 m (62.5 mm), pressure Q18.2 Pa and temperature Q8.4 C (62.5 mC), the
nearest step, plus `OFF_H` (1 m), `OFF_P` (4 Pa) and `OFF_T` (0.0625 C). A
new value shows at the next acquisition. There are no regions and no
faults. `reset()` restores the defaults and the power-on registers, in
standby.

## Behaviour

- **Acquisition.** In active mode one sample every 2^`ST` s, no faster than
  the oversampling ratio allows (Table 46: 6 ms at OS 1 to 512 ms at OS 128).
  Going active flushes the FIFO and zeroes the data registers. `OST` in
  standby takes one sample and clears itself.
- **FIFO disabled.** The sample lands in `OUT_P`/`OUT_T`, `DR_STATUS` (and
  its alias at 00h) reads `PTDR|PDR|TDR`, and `SRC_DRDY` is set.
- **FIFO enabled.** The sample lands in the next FIFO slot; at 32 the FIFO
  has overflowed: `F_OVF`, and `SRC_FIFO` on the rising edge. Full-stop mode
  (`F_MODE` 2) then drops new samples, circular mode (1) drops the oldest.
  `F_WMRK` non-zero sets `F_WMRK_FLAG` and `SRC_FIFO` at the watermark.
  Disabling the FIFO flushes it.
- **Interrupts.** A source enabled in `CTRL_REG4` drives INT1 if `CTRL_REG5`
  routes it there, INT2 otherwise. A read of `F_STATUS` (at 0Dh or its
  alias) clears `SRC_FIFO`; the worker sees the read on its next
  millisecond and releases the pin, so a level-triggered DUT may take the
  interrupt once more.

Two things differ from the part. Reading the FIFO does not empty it: the
bytes stay, and the model takes the `F_STATUS` read as the end of the drain
(count back to zero), as a driver reads it right before or after the burst;
a second drain in the millisecond before the pin is released reads the same
samples again, where the part would return 00h. And `SRC_DRDY` and
`DR_STATUS` never clear (the part clears them when the data is read); the
overwrite bits, the threshold, window and change interrupts, `TIME_DLY`,
min/max capture and the delta registers are not modelled.

## Register coverage

Every register in Table 10 is there with its reset value: 0 except
`WHO_AM_I` (C4h) and `BAR_IN` (C5E7h). `F_SETUP` and 13h..2Dh are
writable; the rest is read-only. `SYSMOD` follows `SBYB`.

## Datasheet

NXP MPL3115A2 data sheet, Rev. 8.1, 17 May 2024: Tables 10-12 (register map and Area A
aliasing), 14.2-14.4 and Tables 14-17 (data formats), 14.7, 14.8 and Tables
21-27 (FIFO), Table 31-32 (`INT_SOURCE`), 14.22 and Tables 44-54 (control
registers, sample rates), 14.23 (offsets).
