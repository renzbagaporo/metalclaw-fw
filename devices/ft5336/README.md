<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# ft5336

FocalTech's FT5336 capacitive touch panel controller on I2C, as the model
`ft5336`: the work-mode register map of FocalTech's CTPM application notes
for the FT5x06/FT5x16 family (`FT5x06_5x16_FTS_AN_CTPM_V1.1.pdf`, 15 Apr
2011, and `FT5x16_AN_CTPM_V1.0.pdf`, 28 May 2012), the map Zephyr's
`focaltech,ft5336` driver reads, as a register map the harness serves from
hardware. One touch point, which the host places and lifts.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, 0x38)` |
| 1 | `int` | `gpio_out` | no | /INT: low while the point is down |

```python
import metalclaw
touch = metalclaw.attach("ft5336", "touch", i2c=(4, 5, 0x38))
touch.position = (120, 200)   # px: X and Y as the registers hold them
touch.pressed = True
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `position` | i32 x2 | rw | px | the point's X and Y, 0-4095 |
| `pressed` | bool | rw | | the point is down |

## Behaviour

- Down: TD_STATUS (02h) 1; TOUCH1_XH-YL (03h-06h) the point, touch ID 0,
  event "put down" until the next millisecond's update, "contact" after
  (2.1.4). Up: TD_STATUS 0 and point 1 reading FFh, touch ID 0Fh.
- Points 2-10 read FFh. DEVICE_MODE 00h (work mode), ID_G_CIPHER 0Ah (the
  FT5x16 note's value, 3.1.44), ID_G_MODE 00h (polling), ID_G_PMODE 00h
  (active), ID_G_STATE 01h.
- /INT, where bound, is low while the point is down (the notes' query
  mode, 1.3).
- Zephyr's driver reports the Y registers as the event's X and the X
  registers as Y (`input_ft5336.c`: col, row).

## Not modelled

More than one point, gestures, the trigger mode's /INT pulse per update,
factory mode, thresholds and calibration, and the FT5336's own chip ID
(its data sheet was not at hand; the cipher is the FT5x16's).
