<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# at24

Atmel/Microchip AT24C02C 2-Kbit I2C serial EEPROM, as the model `at24c02`:
256 bytes behind an 8-bit word address, a register map the harness serves
from hardware (data sheet Atmel-8700G-SEEPROM 01/2015, `AT24C02C.pdf`).

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 50h-57h by A2-A0 |

```python
import metalclaw
eeprom = metalclaw.attach("at24c02", "eeprom", i2c=(4, 5, 0x50))
eeprom.write_region("array", 0, b"hello")   # as a programmer left it
eeprom.read_region("array", 0, 16)
eeprom.writes
```

## Properties and regions

| name | type | access | meaning |
| --- | --- | --- | --- |
| `writes` | u32 | ro | bytes the bus wrote since attach or reset |

| region | size | meaning |
| --- | --- | --- |
| `array` | 256 | the memory, rw |

## Behaviour

At attach every byte reads FFh. A write's first byte after the device
address is the word address; the bytes after it land from there on, and a
read starts at the address the last write selected (or the one after the
last byte read) and runs on. `reset()` is a power cycle and keeps the
array, which is non-volatile; `reset(True)` erases it.

## Not modelled

The 8-byte page write's roll-over within its page (a write runs on to the
next page), the 5 ms write cycle and the NACKs it answers with while
writing, and the write-protect pin.
