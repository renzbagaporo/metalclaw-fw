<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# sct2024

StarChips' SCT2024 16-bit serial-in, parallel-out constant-current LED
driver, as the model `sct2024`. Data sheet V01_01, Feb/08
(`SCT2024_V01_01.pdf`).

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `spi` | `spi_sink` | yes | SDI and CLK, from `(sck, mosi, cs)`; the part has no chip select, so `cs` is the controller's, which frames the capture |
| 1 | `la` | `gpio_in` | yes | LA/, the latch strobe |
| 2 | `oe` | `gpio_in` | no | OE/, the output enable |

```python
import metalclaw
leds = metalclaw.attach("sct2024", "leds", spi=(18, 19, 17), la=(20,), oe=(21,))
leds.outputs, leds.enabled
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `shift` | u32 | ro | the last 16 bits shifted in, the first in bit 15 |
| `outputs` | u32 | ro | what the latch holds: OUTk in bit k |
| `enabled` | bool | ro | OE/ low (or unbound) |
| `latches` | u32 | ro | LA/ pulses seen |

## Behaviour

- SDI is shifted in at each rising CLK edge, MSB first.
- While LA/ is high the latch passes the shift register to the outputs;
  when LA/ falls it holds what the shift register had then.
- `reset()` clears the shift register and the latch.

## Not modelled

The output currents and REXT, SDO (no part is chained), and the shift
register moving while chip select is high.
