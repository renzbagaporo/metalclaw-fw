<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# bmx280

Two Bosch environmental sensors from one source: `bmp280`, a barometer, and
`bme280`, the same barometer with a humidity channel. Either answers on I2C
or SPI with the part's own register map. The host sets a pressure in
pascals, a temperature in millidegrees and, on the BME280, a relative
humidity in thousandths of a percent; the model finds the raw ADC words that
the datasheets' compensation formulas turn back into those values, with the
part's trimming words, and the DUT's driver reads what was set.

The BME280 is register-compatible with the BMP280 for pressure and
temperature (BST-BME280-DS001 5.2); the differences the models carry are the
chip id (`0x58` and `0x60`), `ctrl_hum` at `0xF2`, the humidity data at
`0xFD..0xFE` and the humidity trimming words.

## Datasheets

- Bosch Sensortec BST-BMP280-DS001-26, revision 1.26, October 2021
  (`bst-bmp280-ds001.pdf`).
- Bosch Sensortec BST-BME280-DS001-24, revision 1.24
  (`bst-bme280-ds002.pdf`; its page footers read revision 1.23, January 2022).

Section and table numbers below are from those revisions: "DS001" is the
BMP280's, "DS002" the BME280's.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | one of the two | an I2C target, from `(sda, scl, address)` |
| 1 | `spi` | `spi_target` | one of the two | an SPI target, from `(sck, mosi, miso, cs)` |

`init` refuses both or neither. The I2C address is `0x76` with SDO low and
`0x77` with it high (DS001 5.2). The part picks SPI mode 0 or 3 from the
clock level at chip select (DS001 5.3); the binding advertises both and
listens in mode 0, the Pico SDK's default, until `spi_mode` says otherwise.
Over SPI the part sees seven address bits, bit 7 being read/write (DS001
5.3), so the model's image holds each register at its address with bit 7
cleared when the SPI binding is the one bound; read data follows the control
byte with no dummy byte, from auto-incremented addresses.

```python
import metalclaw
baro = metalclaw.attach("bmp280", "baro", i2c=(4, 5, 0x76))
env = metalclaw.attach("bme280", "env", spi=(18, 19, 16, 17))
baro.pressure = 98765
baro.temperature = 23450
env.humidity = 45670
env.power_mode                       # what the DUT left in ctrl_meas[1:0]
env.stats("spi")
env.reset()
```

## Properties

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `pressure` | i32 | rw | Pa | 30 000 .. 110 000 | the pressure the part measures (DS001 table 2's operating range) |
| `temperature` | i32 | rw | mC | -40 000 .. 85 000 | the temperature it measures (table 2), rounded to the formulas' 0.01 degC |
| `power_mode` | u32 | ro | | 0 .. 3 | `ctrl_meas.mode` as the DUT wrote it: 0 sleep, 1 or 2 forced, 3 normal |
| `spi_mode` | u32 | rw | | 0 or 3 | the SPI mode the target listens in; `ENODEV` on an I2C instance |
| `humidity` | i32 | rw | m%RH | 0 .. 100 000 | `bme280` only: the relative humidity it measures |

Defaults: 101 325 Pa, 25 000 mC, 50 000 m%RH. `reset()` restores them and
rebuilds the register map at power-on.

## How the raw words are found

`bmx280_comp.c` holds the formulas exactly as the datasheets print them
(DS001 3.11.3 and 8.2, DS002 4.2.3 and 8.2) and their inverses. Temperature
first: a bisection over the 20-bit `adc_T` and a scan of the words around
it give the one whose `bmp280_compensate_T_int32` is the set value; its
`t_fine` then feeds the other two. Pressure: of the `adc_P` words the
32-bit fixed-point formula (DS001 8.2, the one pico-examples uses) maps
nearest the set value, the one the 64-bit formula (3.11.3) puts nearest it
too. Humidity: the 16-bit `adc_H` whose `bme280_compensate_H_int32` (Q22.10)
is nearest the set value.

What a driver gets back (`test/unit/bmx280_check.c` sweeps the ranges):

- temperature: exactly the set value, in 0.01 degC;
- pressure, 32-bit formula: exactly the set pascal for about 99 in 100
  values, one off for the rest, which its integer steps skip (875 of
  80 001 at 25 degC);
- pressure, 64-bit formula: within the two formulas' own disagreement, under
  1 Pa from 25 degC up and up to 3.1 Pa below 0 degC;
- humidity: within half the step between neighbouring raw words, at most
  7/1024 %RH.

The trimming words (`calib00..calib25` at `0x88..0xA1`, `calib26..` at
`0xE1..0xE7`, DS001 table 17 and DS002 table 16) are one fixed set: `dig_T1`
.. `dig_P9` = 27504, 26435, -1000, 36477, -10685, 3024, 2855, 140, -7,
15500, -14600, 6000, the example set Bosch printed in earlier revisions of
DS001's section 3.12 (revision 1.26 no longer carries it); `dig_H1` ..
`dig_H6` = 75, 362, 0, 313, 50, 30, values typical of production parts, not
from the datasheet. Any realistic set gives the same results, since the
inversion uses whatever set the part holds; this one keeps every
intermediate of the 32-bit formulas inside 32 bits over the property ranges
(the host check runs with a signed-overflow trap).

## Measurements

A write to `ctrl_meas` with a mode other than sleep sets `status.measuring`
and starts a measurement; the next simulation-clock tick (at most 1 ms)
finishes it: the data registers take the raw words, `measuring` clears, and
forced mode falls back to sleep (DS001 3.6.2). A skipped channel
(`osrs_t`, `osrs_p` or `osrs_h` = 0) reads `0x80000` (`0x8000` for humidity)
(DS001 tables 4 and 5, DS002 table 20). `ctrl_hum` takes effect at the next
`ctrl_meas` write (DS002 5.4.3). In normal mode a property change shows at
the next tick; in sleep the data registers keep the last measurement, and
before the first one they read their reset values (DS001 table 18).
Writing `0xB6` to `reset` is a power-on reset (DS001 4.3.2); `reset` reads
`0x00`.

## Register coverage

| reg | name | value | access | from |
| --- | --- | --- | --- | --- |
| 0x88..0xA1 | `calib00..calib25` | the trimming words; `0xA1` is `dig_H1` on the BME280 | ro | DS001 table 17, DS002 table 16 |
| 0xD0 | `id` | `0x58` / `0x60` | ro | DS001 4.3.1, DS002 5.4.1 |
| 0xE0 | `reset` | `0x00` | w: `0xB6` resets | DS001 4.3.2 |
| 0xE1..0xE7 | `calib26..calib32` | `dig_H2..dig_H6` (BME280) | ro | DS002 table 16 |
| 0xF2 | `ctrl_hum` | `0x00`, bits 2:0 | rw (BME280) | DS002 5.4.3 |
| 0xF3 | `status` | `measuring` while a measurement is pending | ro | DS001 4.3.3 |
| 0xF4 | `ctrl_meas` | `0x00` | rw | DS001 4.3.4 |
| 0xF5 | `config` | `0x00`, bit 1 reads 0 | rw | DS001 4.3.5 |
| 0xF7..0xF9 | `press` | `0x80 0x00 0x00` until measured | ro | DS001 4.3.6 |
| 0xFA..0xFC | `temp` | `0x80 0x00 0x00` until measured | ro | DS001 4.3.7 |
| 0xFD..0xFE | `hum` | `0x80 0x00` until measured | ro (BME280) | DS002 5.4.9 |

Everything else reads `0x00` and ignores writes.

## Not modelled

- The data are always 20-bit (16-bit humidity) words. A real part resolves
  16 + `osrs` - 1 bits without the IIR filter (DS001 3.3.1, DS002 5.2) and
  so could not produce every pascal at low oversampling; the model can.
- The IIR filter, standby timing (`t_sb`) and measurement durations
  (DS001 3.8): a measurement lands on the next millisecond tick.
- Multi-register writes. The datasheets write in register/data pairs
  (DS001 5.2.1 and 5.3.1), but both target engines treat the bytes after
  the address as data for consecutive registers, so a pair write puts its
  second address byte into the next register. pico-examples writes one
  register per transaction.
- Shadowing of the data registers during a burst read (DS001 3.10): a
  property change overlapping a burst can mix old and new bytes.
- 3-wire SPI: `spi3w_en` is stored only.
