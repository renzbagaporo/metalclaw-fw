<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# mcp4728

Microchip's MCP4728 quad 12-bit DAC with EEPROM on I2C, as the model
`mcp4728`: the write commands of Table 5-1 decoded into each channel's input
register and EEPROM (data sheet DS22187E, `MCP4728.pdf`). The part is a
sink: the harness captures what the host writes and the model decodes it.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, 0x60)` (A2-A0 000 from the factory) |

```python
import metalclaw
dac = metalclaw.attach("mcp4728", "dac", i2c=(4, 5, 0x60))
dac.codes      # (A, B, C, D), 0-4095
dac.voltages   # mV at each VOUT
dac.vdd = 5000 # mV, the reference for channels on VDD
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `codes` | u32 x4 | ro | | each channel's D11-D0 |
| `voltages` | u32 x4 | ro | mV | each output: code / 4096 of 2.048 V (x2 with GX) on the internal reference, of `vdd` on VDD; 0 powered down |
| `vdd` | u32 | rw | mV | the supply, 3300 at attach |
| `writes` | u32 | ro | | channel updates and select-bit writes since attach |

## Behaviour

- Fast Write (C2-C1 00): PD and D for channels A to D in pairs, round
  again after D.
- Multi-Write (01000): a command byte with DAC1-DAC0, then VREF, PD, GX
  and D, repeated per channel.
- Sequential Write (01010) from the selected channel to D, and Single Write
  (01011): the input registers and the EEPROM.
- Write VREF (100), Gain (110) and Power-Down (101) selection bits.
- LDAC is taken as held low, so every write reaches the outputs at once
  and UDAC makes no difference.
- At attach, and on `reset()`, the input registers load from EEPROM;
  `reset(True)` first restores Table 4-2's factory EEPROM (internal
  reference, normal mode, gain 1, code 0).

## Not modelled

Reads return Figure 5-15's first byte (C0h: ready, powered, channel A,
address 000) for every byte rather than the 24-byte register and EEPROM
dump; the address-bit write, which takes the LDAC handshake; the general
calls; the LDAC and RDY/BSY pins; the EEPROM write time.
