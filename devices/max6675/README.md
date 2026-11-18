<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# max6675

Maxim's MAX6675 cold-junction-compensated K-thermocouple-to-digital
converter on SPI, as the model `max6675` (data sheet 19-2235 Rev 1,
`MAX6675.pdf`, as Adafruit hosts it). The part is read-only: every
chip-select window clocks out one 16-bit frame, which the harness serves as
an output stream.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `spi` | `spi_target` | yes | an SPI target, from `(sck, mosi, miso, cs)`; the part has no SI, so MOSI goes unused |

SPI mode 0 (or 3), MSB first; the part runs to 4.3 MHz.

```python
import metalclaw
tc = metalclaw.attach("max6675", "tc", spi=(18, 19, 16, 17))
tc.temperature = 123250   # m C
tc.open = True            # the thermocouple input open
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `temperature` | i32 | rw | m C | the compensated thermocouple temperature, 0 to 1023.75 C, 25 C at attach |
| `open` | bool | rw | | the thermocouple input is open (D2) |
| `spi_mode` | u32 | rw | | 0 or 3 |

## Frame

Figure 2 and Serial Interface: D15, a dummy sign bit, reads 0; D14-D3 hold
the temperature in 0.25 C counts, rounded; D2 reads 1 with the input open;
D1, the device ID, reads 0; D0, three-state on the part, reads 0.

## Not modelled

The 0.22 s conversion and its restart when CS falls (every frame reads the
present temperature), and the cold-junction compensation itself (the
property is the result).
