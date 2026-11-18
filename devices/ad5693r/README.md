<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# ad5693r

Analog Devices' AD5693R 16-bit nanoDAC+ on I2C, as the model `ad5693r`:
Table 12's three-byte writes decoded into its input, DAC and control
registers (data sheet Rev. D, `AD5693R.pdf`). The part is a sink: the
harness captures what the host writes and the model decodes it. Zephyr
drives it as `adi,ad5693`.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 4Ch with A0 low, 4Eh high (Table 11) |

```python
import metalclaw
dac = metalclaw.attach("ad5693r", "dac", i2c=(4, 5, 0x4c))
dac.code      # the DAC register, 0-65535
dac.voltage   # mV at VOUT
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `code` | u32 | ro | | the DAC register, which drives VOUT |
| `input` | u32 | ro | | the input register |
| `control` | u32 | ro | | the control register's PD1-PD0, REF and gain bits (D14-D11) |
| `voltage` | u32 | ro | mV | code / 65536 of the reference (2.5 V internal, `vref` with REF set), x2 with gain; 0 powered down |
| `vref` | u32 | rw | mV | an external reference, used with the internal one off; 0 at attach |
| `writes` | u32 | ro | | commands other than NOP since attach |

## Behaviour

- Each window carries a command byte and two data bytes (Figure 47),
  repeated: 1 writes the input register, 2 copies it to the DAC register
  (the software LDAC), 3 writes both, 4 the control register, whose reset
  bit restores the defaults (zero scale, normal mode, internal reference,
  gain 1). LDAC is taken as held high.
- At attach and on `reset()` the registers are at those defaults.

## Not modelled

A read returns the input register's high byte for both bytes (the
harness's sink answers one byte for every byte read): exact for 0000h, the
value Zephyr's driver reads back after its software reset. The NACK the
part gives a reset request, the LDAC and RESET pins, and the settling and
power-down exit times.
