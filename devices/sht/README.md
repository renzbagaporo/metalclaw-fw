<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# sht

Sensirion humidity and temperature sensors on I2C, two models: `sht3x`, the
SHT3x-DIS (datasheet version 7), and `sht4x` (Sensirion SHT4x datasheet). Both take commands rather than registers: the
host writes a command (16 bits on the SHT3x, 8 on the SHT4x) and then
reads its answer, each 16-bit word followed by its CRC-8.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 44h or 45h (SHT3x ADDR), 44h (SHT4x-A) |

```python
import metalclaw
rh = metalclaw.attach("sht4x", "rh", i2c=(4, 5, 0x44))
rh.temperature = 23500   # m C
rh.humidity = 45000      # m %RH
rh.commands
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `temperature` | i32 | rw | m C | what every measurement reads, 25 C at attach |
| `humidity` | i32 | rw | m %RH | likewise, 50 %RH at attach |
| `commands` | u32 | ro | | command words written since attach or reset |

## Answers

The harness serves the part as a register map whose pointer map sends a
command's first byte to the answer it reads, so every answer comes from
hardware with no model call:

- the measurement, T then RH (SHT3x 4.13, SHT4x 4.6: T = -45 + 175 x raw /
  65535; RH = 100 x raw / 65535 on the SHT3x, -6 + 125 x raw / 65535 on the
  SHT4x), for every measurement and fetch command (SHT3x single shot,
  periodic and Fetch Data E000h; SHT4x precision and heater commands);
- the serial number for Read Serial Number (SHT3x 3780h, SHT4x 89h), a
  fixed value per model;
- the status word, 0000h, for the SHT3x's Read Status F32Dh.

Each word carries its CRC-8 (polynomial 31h, init FFh; SHT3x 4.12, SHT4x
4.4). A 16-bit command's second byte lands in the map where the first
points, and the model puts the answers back at once.

## Not modelled

Measurement timing (a read before the measurement time answers, where the
part NACKs), clock stretching, the SHT3x's periodic rates, ART, heater,
alert limits and ALERT pin, the status bits, the SHT4x heater's effect on
the reading, and the CRC check of commands the host sends with data.
