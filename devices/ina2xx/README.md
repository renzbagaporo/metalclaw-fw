<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# ina2xx

Texas Instruments current and power monitors on I2C. Four models come out of
one file: `ina219`, `ina228`, `ina237` and `ina260`. The harness answers at
the part's address with the part's register map; the host sets the world on
the part's pins (the bus voltage, the current through the shunt, the shunt's
resistance, the die temperature) and the model turns it into the ADC codes
the datasheet gives, then computes `CURRENT`, `POWER` and, on the INA228,
`ENERGY` and `CHARGE` from those codes and the calibration register the DUT
wrote, with the datasheet's own equations. A DUT that programs the wrong
calibration reads the wrong current, as it would from the real part.

The parts' registers are 16 to 40 bits wide behind one pointer value each,
most significant byte first. The model lays them out back to back in a
`struct mclw_regmap` and gives the map a pointer map (`regmap.h`), so the
I2C engine starts each transfer at the register's first byte; every pointer
value the part does not implement reads zeros. Writes land per byte; the
model's `on_write` acts once a register's last writable byte has landed,
restores the bits the datasheet makes read-only, and recomputes in interrupt
context. Conversion timing runs on the worker in `time_advanced`.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from the tuple `(sda, scl, address)` |

The address comes from the board: 0x40 with A0 and A1 grounded on every
part, which is what the pico-examples programs use. The ALERT pin is not
modelled.

```python
import metalclaw
ina = metalclaw.attach("ina219", "ina", i2c=(4, 5, 0x40))
ina.bus_voltage = 5124          # mV at IN- (the load side)
ina.current = 123400            # uA through the shunt, IN+ to IN-
ina.shunt_resistance = 100000   # uohm: Adafruit's 0.1 ohm breakout
ina.stats("i2c")
ina.reset()
ina.detach()
```

## Properties

| name | type | unit | range | models | meaning |
| --- | --- | --- | --- | --- | --- |
| `bus_voltage` | i32 | mV | 0 .. 85 000 | all | the bus voltage the part measures |
| `current` | i32 | uA | -100 000 000 .. 100 000 000 | all | the current through the shunt; negative flows IN- to IN+ |
| `shunt_resistance` | i32 | uohm | 1 .. 10 000 000 | `ina219`, `ina228`, `ina237` | the external shunt; the shunt voltage is current x resistance |
| `temperature` | i32 | mC | -40 000 .. 125 000 | `ina228`, `ina237` | the die temperature |

Defaults: 0 mV, 0 uA, 25 000 mC, and the shunt of the breakout the
pico-examples program assumes: 100 000 uohm on the INA219, 15 000 uohm on
the INA228 and INA237. The INA260's 2 mohm shunt is inside the part; it
reports current directly.

Codes are the nearest LSB, saturated at the range the configuration selects
(the INA219's PGA and BRNG, the INA228/INA237's ADCRANGE). In a continuous
mode a property write shows at once; in a triggered mode it shows at the
next conversion. There are no regions and no faults. `reset()` puts the
properties back to their defaults and every register to its power-on value.

## Arithmetic

| part | shunt voltage | bus voltage | current | power |
| --- | --- | --- | --- | --- |
| INA219 | 10 uV/LSB, sign-extended over the PGA range | 4 mV/LSB in bits 15:3, CNVR bit 1, OVF bit 0 | shunt x CAL / 4096 (Eq. 4), CAL bit 0 void | \|current\| x bus / 5000 (Eq. 5) |
| INA228 | 312.5 or 78.125 nV/LSB, 20 bits in 23:4 | 195.3125 uV/LSB, 20 bits in 23:4 | VSHUNT x 4096 / SHUNT_CAL | \|current\| x VBUS / 16384 |
| INA237 | 5 or 1.25 uV/LSB, 16 bits | 3.125 mV/LSB | VSHUNT x 4096 / SHUNT_CAL | \|current\| x VBUS / 64 |
| INA260 | (current at 1.25 mA/LSB) | 1.25 mV/LSB | | \|current\| x bus / 6400 at 10 mW/LSB |

The INA228 and INA237 rows follow from their equations: SHUNT_CAL is
13107.2e6 (INA228) or 819.2e6 (INA237) x CURRENT_LSB x RSHUNT, times 4 at
ADCRANGE 1, and the shunt LSB times that constant is 4096 in both ranges;
POWER is 3.2 or 0.2 x CURRENT_LSB per LSB. The INA228 example of SLYS021A
Table 8-4 (VSHUNT 311040, SHUNT_CAL 4050, CURRENT 314572) checks out with
truncation. Current and power read zero until the calibration register is
non-zero. A result that does not fit sets the overflow flag (INA219 OVF,
INA228/INA237 MATHOF, INA260 OVF) and saturates.

INA228 only: each completed conversion adds POWER x t / 16 to `ENERGY` and
CURRENT x t to `CHARGE` (t in seconds, Eq. 6 and 7, 7.3.2); both are 40-bit
and roll over, setting ENERGYOF or CHARGEOF; CONFIG.RSTACC clears them.
`DIETEMP` is 7.8125 m°C/LSB (16 bits) on the INA228 and 125 m°C/LSB in bits
15:4 on the INA237.

## Conversions

A write to the mode register (CONFIG on the INA219 and INA260, ADC_CONFIG on
the INA228 and INA237) clears the conversion-ready flag and restarts. In a
continuous mode the results publish at once and then every conversion
cycle, each setting the flag; a triggered mode publishes once, a cycle after
the write, sets the flag and stops. A cycle is the enabled channels'
conversion times times the averaging count, from the datasheet tables
(INA219 Table 5, INA228/INA237 Table 7-6, INA260 Table 5), to the worker's
millisecond. `RST` (CONFIG bit 15) resets every register and self-clears.

The flag is the INA219's CNVR, the INA228/INA237's CNVRF and the INA260's
CVRF. Reading DIAG_ALRT clears CNVRF, which the map does by clearing the
register's low byte on read; MEMSTAT in the same byte reads 0 until the
worker's next millisecond puts it back. The INA219's CNVR does not clear on
a read of POWER, nor the INA260's CVRF on a read of Mask/Enable: the engine
does not tell the model about reads.

## Register coverage

Every register in the datasheet's register table is there, with its reset
value; the bits the datasheet marks read-only or reserved keep their reset
value whatever the bus writes. The limit and alert registers store what is
written and compare against nothing.

| model | registers | identity |
| --- | --- | --- |
| `ina219` | 00h-05h | none |
| `ina228` | 00h-11h, 3Eh, 3Fh | MANUFACTURER_ID `5449h`, DEVICE_ID `2281h` |
| `ina237` | 00h-02h, 04h-08h, 0Bh-11h, 3Eh | MANUFACTURER_ID `5449h` |
| `ina260` | 00h-03h, 06h, 07h, FEh, FFh | Manufacturer ID `5449h`, Die ID `2270h` |

A read that runs past a register's last byte goes on into the next
register in the layout; the parts' behaviour there is not documented.

## Datasheets

In `.dev/resources/datasheets/` of the main checkout:

- INA219: SBOS448G, December 2015 (`ina219_sbos448g.pdf`): Table 2, 8.5,
  Equations 1-5, 8.6.2-8.6.4, Tables 3-7.
- INA228: SLYS021A, May 2022 (`ina228_slys021a.pdf`): 7.3.2, Tables 7-3 to
  7-24, 8.1.2 Equations 2-7, Table 8-1, Table 8-4.
- INA237: SBOSA20A, May 2022 (`ina237_sbosa20a.pdf`): 7.3.1, 7.3.2, Tables
  7-3 to 7-20, 8.1.2 Equations 1-4, Table 8-1.
- INA260: SBOS656C, December 2016 (`ina260_sbos656c.pdf`): 8.5.1, Table 1,
  Table 4, 8.6.1-8.6.8.
