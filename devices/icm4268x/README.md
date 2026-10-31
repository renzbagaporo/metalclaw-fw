<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# icm4268x

A TDK InvenSense ICM-42688 or ICM-42686 six-axis IMU on SPI. Two models come
out of one file, `icm42688` and `icm42686`, differing only in `WHO_AM_I`. The
harness answers the part's register map over SPI, so the board under test can
bind Zephyr's in-tree `invensense,icm4268x` driver to it and read acceleration,
angular rate and die temperature through the sensor API. The host sets the raw
16-bit counts the part would report; the DUT reads back what was set.

The model is one file with no bus code. It fills a four-bank
`struct mclw_regmap`, tells the engine how the part frames a register
access (read bit `0x80`, seven address bits, auto-increment, no dummy bytes)
and arms the SPI target engine with it. From then on every window is served
from DMA; the model is called back only through `on_write`, in interrupt
context, for the two registers with side effects.

A property write changes the sample and the image in one critical section,
so `on_write` (a soft reset rebuilds the image from the sample) never sees
half of one. A bus read gets no such promise: the engine's DMA reads the
live image while armed, so a burst that overlaps a property write can carry
bytes of the old sample and of the new one.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `spi` | `spi_target` | yes | an SPI target, from the tuple `(sck, mosi, miso, cs)` |

The binding advertises the datasheet's modes 0 and 3, mode 3 by default (which
is what Zephyr's driver forces), MSB first. On fixture 1 that is SCK on GP18,
MOSI on GP19, MISO on GP16 and chip select on GP17:

```python
import metalclaw
imu = metalclaw.attach("icm42688", "imu", spi=(18, 19, 16, 17))
imu.set(accel=(2048, -2048, 1024), gyro=(164, -164, 8200), temperature=1325)
imu.power_mode, imu.accel_config, imu.gyro_config, imu.bank, imu.resets
imu.stats("spi")
imu.reset()
imu.detach()
```

## Properties

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `accel` | i32[3] | rw | LSB | -32768 .. 32767 | `ACCEL_DATA_X1..Z0`, big-endian per axis |
| `gyro` | i32[3] | rw | LSB | -32768 .. 32767 | `GYRO_DATA_X1..Z0` |
| `temperature` | i32 | rw | LSB | -32768 .. 32767 | `TEMP_DATA1..0`; the driver reports `raw / 132.48 + 25` C |
| `spi_mode` | u32 | rw | | 0 or 3 | the SPI mode the target listens in |
| `power_mode` | u32 | ro | | 0 .. 255 | `PWR_MGMT0` as the DUT left it; `0x0F` is low-noise on both sensors |
| `accel_config` | u32 | ro | | 0 .. 255 | `ACCEL_CONFIG0` as written |
| `gyro_config` | u32 | ro | | 0 .. 255 | `GYRO_CONFIG0` as written |
| `bank` | u32 | ro | | 0 .. 3 | the register bank the last `REG_BANK_SEL` write selected |
| `resets` | u32 | ro | | | soft resets the DUT issued through `DEVICE_CONFIG` |

Counts are the datasheet's raw words; the conversion to physical units is
the driver's and depends on the full scale the DUT configures, so the cases
apply the driver's own formulas at the overlay's full scales (2048 LSB/g,
16.4 LSB/dps). There are no regions and no faults.

`reset()` zeroes the sample and rebuilds the map at power-on values, throwing
away the DUT's configuration; `factory` changes nothing further.

## Register coverage

Four banks of 256 bytes, filler `0x00`, every register writable and zero
except:

| bank | reg | name | value | why |
| --- | --- | --- | --- | --- |
| 0 | 0x1D..0x2A | `TEMP_DATA1..GYRO_DATA_Z0` | the sample | the 14-byte burst `sensor_sample_fetch` reads |
| 0 | 0x2D | `INT_STATUS` | `0x10` after reset | `RESET_DONE`; `DATA_RDY` (`0x08`) is added once `PWR_MGMT0` turns a sensor on |
| 0 | 0x4D | `INTF_CONFIG1` | `0x91` | reset value |
| 0 | 0x4F, 0x50 | `GYRO_CONFIG0`, `ACCEL_CONFIG0` | `0x06` | reset values; the driver overwrites both |
| 0 | 0x54 | `TMST_CONFIG` | `0x23` | the driver read-modify-writes it |
| 0 | 0x64 | `INT_CONFIG1` | `0x10` | reset value |
| 0 | 0x75 | `WHO_AM_I` | `0x47` or `0x44` | read-only; the driver refuses anything else |
| 0 | 0x76 | `REG_BANK_SEL` | | handled by the engine: the written bank applies to the next window |

Two writes have side effects, both done in `on_write` from interrupt context:
`DEVICE_CONFIG` bit 0 rebuilds the whole map at power-on values (the soft
reset the driver opens with, counted in `resets`), and `PWR_MGMT0` with any
sensor on raises `DATA_RDY`. `INT_STATUS` is not clear-on-read: the fixture
has no data rate, so a fetch is always ready.

## The Zephyr driver this is built against

Zephyr v4.4.0, `drivers/sensor/tdk/icm4268x/`, SPI, with
`CONFIG_ICM4268X_STREAM=n` and `CONFIG_ICM4268X_TRIGGER_NONE=y`. Its init is
three windows of reset (write `DEVICE_CONFIG`, check `RESET_DONE`, check
`WHO_AM_I`) and fifteen of configuration, including a hop into bank 1 for
`INTF_CONFIG5` and back. `sensor_sample_fetch` reads `INT_STATUS`, fails with
`EBUSY` without `DATA_RDY`, then reads the 14-byte burst from `TEMP_DATA1`.

## The DUT image

`test/dut/rp2.overlay` puts `spi0` on GP18/GP19/GP16 with a GPIO chip select on
GP17 (the PL022's own chip select drops whenever its transmit FIFO drains, and
the driver splits every read into a command chunk and a data chunk), an
`icm42688@0` node aliased `imu` in mode 3 at 10.7 MHz, low-noise power modes,
50 Hz data rates and the ±16 g / ±2000 dps full scales. It asks for a 1 ms
`spi-cs-setup-delay-ns`: the driver's reset sequence reads back-to-back, and
the engine needs about 15 µs after a release before it can serve the next
window; the setup delay lets it join a window that has already been asserted.

The DUT command is `SENSOR imu accel_xyz gyro_xyz die_temp [count=N]`: `N`
fetches through the driver, the first reading's channels in the sensor API's
micro-units, and how many later fetches differed from it.

```sh
just build-dut icm4268x
METALCLAW_HARNESS_SERIAL=<serial> METALCLAW_ARM_TOOLCHAIN_DIR=<dir> \
METALCLAW_DUT_DEVICE=icm4268x just test hardware
```

## Tests

`test/cases.py` attaches `icm42688` on the rig's SPI pins for this part
(`(18, 19, 16, 17)` on fixture 1, from the platform's `fixture` description),
resets the DUT and checks: four samples across the range come back through the driver within one
LSB on every axis; the driver left `PWR_MGMT0`, `ACCEL_CONFIG0` and
`GYRO_CONFIG0` at the overlay's values, bank 0 selected, at least one soft
reset counted; ten fetches move at least twenty transactions; a thousand
fetches all agree with no stale windows and no DMA faults; a DUT reset counts
another soft reset and the driver comes back configured; a `reset()` from the
harness clears the configuration and the next DUT reset restores it.

## Not modelled

The FIFO, interrupts, timestamps, data rates and full scales affecting the
sample, and the APEX features. Nothing the driver does in this configuration
depends on them.
