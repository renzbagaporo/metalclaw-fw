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
| 1 | `int1` | `gpio_out` | no | the INT1 pin |

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
| `fifo_count` | u32 | ro | bytes | 0 .. 2048 | bytes the FIFO holds |
| `fifo_lost` | u32 | ro | packets | 0 .. 65535 | packets dropped for want of room, as `FIFO_LOST_PKT0..1` |

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
| 0 | 0x2D | `INT_STATUS` | `0x10` after reset | `RESET_DONE`; `DATA_RDY` (`0x08`) is added once `PWR_MGMT0` turns a sensor on; `FIFO_THS` (`0x04`) and `FIFO_FULL` (`0x02`) from the FIFO |
| 0 | 0x2E, 0x2F | `FIFO_COUNTH..L` | the FIFO's count | read-only; see the FIFO |
| 0 | 0x30 | `FIFO_DATA` | the FIFO | a read here streams the FIFO, not the image |
| 0 | 0x4B | `SIGNAL_PATH_RESET` | `0x00` | write-to-clear; `FIFO_FLUSH` empties the FIFO |
| 0 | 0x4C | `INTF_CONFIG0` | `0x30` | reset value; sets the FIFO count's unit and byte order |
| 0 | 0x6C, 0x6D | `FIFO_LOST_PKT0..1` | packets lost | read-only, low byte first |
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
500 Hz data rates and the ±16 g / ±2000 dps full scales. It asks for a 1 ms
`spi-cs-setup-delay-ns`: the driver's reset sequence reads back-to-back, and
the engine needs a gap of about 12 µs after a release before it can serve the next
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

The `icm4268x/fifo` image (`test/dut/fifo/`) has no driver: the IMU is a
bare SPI device in mode 3 with the same GPIO chip select and 1 ms setup
delay, and the cases clock its registers through `RAW` at 1 MHz (`RAW`
sends the command byte and the data in one go, with none of the pause the
driver's two chunks leave for the engine's register lookup). They fill the
FIFO at 12.5 Hz, switch the sensors off, and check every packet byte for
byte, the 80 ms timestamp step, the count falling by a packet per read,
`0xFF` once empty, and eight reads in a row each taking the next packet.

`test/native/test_icm4268x_fifo.py` checks the FIFO over the host
harness's bridge: packets 1, 3 and 4 byte for byte, the timestamp step, the
count in bytes and records and either byte order, pops whole and partial, a
flush, bypass mode, the watermark, and the packets lost once full.

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

Full scales affecting the sample, FSYNC, the APEX features, and the
FIFO's read cache (6.3: up to 2048 bytes are stored, never the extra
packets the cache can hold during a read).

## The FIFO

DS-000347 Revision 1.2, section 6 and 14.4, 14.22-14.24, 14.33, 14.34,
14.45-14.47, 14.55-14.56. With `FIFO_CONFIG`'s mode stream-to-FIFO or
STOP-on-FULL and a sensor on, a packet goes in at every ODR period; at
ODRs above 1 kHz a millisecond tick adds the several that fell in it,
timestamped a period apart (32 at most, after which the FIFO starts again
from the tick). `FIFO_CONFIG1` picks the packet (6.1):

| `FIFO_CONFIG1` | packet | bytes | header |
| --- | --- | --- | --- |
| `ACCEL_EN` | 1: accel, temperature | 8 | `0x40` |
| `GYRO_EN` | 2: gyro, temperature | 8 | `0x20` |
| both | 3: accel, gyro, temperature, timestamp | 16 | `0x68` |
| `HIRES_EN` | 4: 20-bit accel and gyro, 16-bit temperature, timestamp | 20 | `0x78` |

Fields are big-endian. The header's ODR-change bits are set on the first
packet after `ACCEL_CONFIG0`'s or `GYRO_CONFIG0`'s ODR changed. A sensor
that is off fills its fields with the invalid value, -32768 or -524288. The
8-bit temperature is `TEMP_DATA / 64` (the datasheet's 2.07 against 132.48
LSB/C). Packet 4 carries the 16-bit sample at the hi-res scales, accel x4
and gyro x8 (8192 LSB/g, 131 LSB/dps), which equal the register values when
the DUT configured ±16 g and ±2000 dps. The timestamp is the low 16 bits of
the microsecond clock, or with `TMST_DELTA_EN` the time since the last
packet; `TMST_RES` divides by 16. A change of packet layout drops the
packets of the old one.

A burst that starts at `INT_STATUS`, `FIFO_COUNTH` or `FIFO_COUNTL` runs on
into `FIFO_DATA` and stays there, popping the packet bytes it read, as a
burst from `FIFO_DATA` does: icm426xx reads status, count and a packet in
one such burst. (The datasheet says the address auto-increments; that the
FIFO port then holds is how the part behaves for that driver.)

`FIFO_COUNTH..L` count bytes, or packets with `INTF_CONFIG0`'s
`FIFO_COUNT_REC`, big-endian unless `FIFO_COUNT_ENDIAN` is cleared. A burst
read of `FIFO_DATA` pops what it read once the window ends; without
`FIFO_RESUME_PARTIAL_RD` a packet read in part goes whole. Reads past the
end of the FIFO return `0xFF`. A FIFO with no room for the next packet
raises `FIFO_FULL`; one that had no room for this one drops it and counts
it in `FIFO_LOST_PKT0..1`, in both modes. `FIFO_THS` rises when the count
reaches `FIFO_CONFIG2..3`'s watermark (in the count's unit), and with
`FIFO_WM_GT_TH` at every packet while it stays there. `FIFO_FLUSH` and a
return to bypass mode empty the FIFO.

The SPI engine serves `FIFO_DATA` from the FIFO's buffer and pops it in the
interrupt at the window's end (`mclw_regmap_set_stream`), like the
clear-on-read registers, so a read that begins within that interrupt's
latency after the last one ended can still see the packets the last one
read.

## Data ready and INT1

INT_SOURCE0's FIFO_THS_INT1_EN and FIFO_FULL_INT1_EN route those events to
INT1 as UI_DRDY_INT1_EN does data ready, pulsed or latched alike.

With either sensor on in PWR_MGMT0, a sample is taken at the accelerometer's
ODR (ACCEL_CONFIG0, DS-000347 Revision 1.2, 14.37), or the gyroscope's when
only it runs, and sets DATA_RDY_INT in INT_STATUS, which clears when read
(14.21). The simulation clock ticks every millisecond, so an ODR above 1 kHz
delivers at 1 kHz. With `int1` bound and INT_SOURCE0's UI_DRDY_INT1_EN set
(14.51), each sample emits a 100 us pulse on INT1, 8 us with INT_CONFIG1's
INT_TPULSE_DURATION (14.50), or in latched mode (INT_CONFIG INT1_MODE, 14.3)
holds INT1 until INT_STATUS is read. INT1_POLARITY sets the active level, and
INT1_DRIVE_CIRCUIT push-pull or open drain, the reset value, which only pulls
INT1 low and otherwise releases it (`mclw_hal_gpio_release`): the line then
sits where its pull leaves it. INT_CONFIG0's clear options other than the
status read are not modelled.

