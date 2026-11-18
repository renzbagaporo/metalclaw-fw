<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# vl53l0x

ST's VL53L0X time-of-flight ranging sensor on I2C, as the model `vl53l0x`.
ST publishes no register map for it, only its API (hal_st's
`vl53l0x_api*.c`), so the registers and handshakes here are the ones that
API and Zephyr's driver use; the identification is what Zephyr's driver
checks.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from `(sda, scl, address)`: 29h after power-on |

```python
import metalclaw
tof = metalclaw.attach("vl53l0x", "tof", i2c=(4, 5, 0x29))
tof.distance = 350
tof.rangings
```

## Properties

| name | type | access | meaning |
| --- | --- | --- | --- |
| `distance` | u32 | rw | mm, what every ranging reports (200 at attach) |
| `rangings` | u32 | ro | rangings and reference calibrations started |

## Behaviour

- C0h-C1h read EEh AAh, C2h 10h.
- Writing bit 0 of SYSRANGE_START (00h, with the page register FFh at 0)
  completes a ranging at once: 00h reads 0 again, RESULT_INTERRUPT_STATUS
  (13h) new sample ready (04h), RESULT_RANGE_STATUS (14h) a valid range
  (status 11) with data ready in bit 0, the range at 1Eh-1Fh and the
  reference peak signal rate at B6h-B7h. SYSTEM_INTERRUPT_CLEAR (0Bh)
  clears them.
- The NVM read the API does: 00h written to 83h reads non-zero at once;
  a command at 94h puts its word at 90h-93h. Command 6Bh gives five
  aperture reference SPADs, 24h and 25h a good-SPAD map with every SPAD
  good.
- Every other register takes what is written.
- `reset()` is a power cycle.

## Not modelled

The ranging itself (signal and ambient rates, sigma, timing budget), the
reference calibrations' results, continuous and timed modes, the
interrupt pin, XSHUT, and an address set with I2C_SLAVE_DEVICE_ADDRESS.
