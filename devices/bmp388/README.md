<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# bmp388

A Bosch BMP388 barometer on I2C or SPI. The harness answers at the part's own
address, or behind its chip select, with the part's own register map, so the
board under test can bind Zephyr's in-tree `bosch,bmp388` driver to it and
read pressure and temperature through the sensor API. The host sets a
physical pressure in pascals and a temperature in millidegrees; the model
turns those into the 24-bit raw words the driver's compensation maths
expects, and the DUT reads back what was set.

The model is one file with no bus code. It fills a `struct metalclaw_regmap`
in `init` and arms whichever target engine was bound with it, the SPI one
with the datasheet's framing (read bit `0x80`, seven address bits, one dummy
byte before read data, auto-increment); from then on the engine serves every
bus read and write from interrupt context without calling back into the
model. The model touches the map again only when a property changes, under
`metalclaw_hal_critical_enter()`, so a six-byte burst read never sees half of
one sample and half of the next.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | one of the two | an I2C target, from the tuple `(sda, scl, address)` |
| 1 | `spi` | `spi_target` | one of the two | an SPI target, from the tuple `(sck, mosi, miso, cs)` |

`init` refuses an attach with both or neither. The SPI binding advertises
modes 0 and 3, mode 3 by default (Zephyr's driver forces it), MSB first. On
fixture 1 that is SDA on GP4, SCL on GP5 and address `0x76`, the address the
BMP388 uses with SDO low, or SCK on GP18, MOSI on GP19, MISO on GP16 and chip
select on GP22:

```python
import metalclaw
baro = metalclaw.attach("bmp388", "baro", i2c=(4, 5, 0x76))
baro = metalclaw.attach("bmp388", "baro", spi=(18, 19, 16, 22))
baro.pressure = 90000
baro.temperature = -12500
baro.transactions, baro.power_mode, baro.osr, baro.odr
baro.stats("spi")
baro.reset()
baro.detach()
```

## Properties

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `pressure` | i32 | rw | Pa | 30 000 .. 125 000 | the pressure the part reports; writing it republishes `DATA_0..DATA_2` |
| `temperature` | i32 | rw | mC | -40 000 .. 85 000 | the temperature the part reports; writing it republishes `DATA_3..DATA_5` |
| `transactions` | u32 | ro | | | transactions the bound engine has served, from the HAL |
| `power_mode` | u32 | ro | | 0 .. 3 | `PWR_CTRL[5:4]` as the DUT left it: 0 sleep, 1 forced, 3 normal |
| `osr` | u32 | ro | | 0 .. 63 | the whole `OSR` register: `osr_p` in bits 2:0, `osr_t` in bits 5:3 |
| `odr` | u32 | ro | | 0 .. 31 | `ODR[4:0]` as the DUT left it; 2 is 50 Hz |
| `spi_mode` | u32 | rw | | 0 or 3 | the SPI mode the target listens in; `ENODEV` to set on an I2C instance |

`pressure` and `temperature` default to 101 325 Pa and 25 000 mC. `power_mode`,
`osr` and `odr` are read straight out of the register map, so they are the
bytes the DUT's driver actually wrote: they are how a case proves the
configuration half of the driver's init ran, not just the data half.

There are no regions and **no faults**. A fault that withholds data-ready would
hang the DUT rather than fail it: `bmp388_sample_fetch()` spins on
`STATUS.drdy_press` with no timeout and no error path. `nak` is not available
either — the engine answers every addressed transfer from the map, and the
model is never on the wire. The old implementation had no fault injection for
this part.

`reset()` puts pressure and temperature back to their defaults and rebuilds the
whole register map at its power-on values, which also throws away everything
the DUT configured; `factory` changes nothing further. The instance is
disarmed for the rebuild and re-armed after it, so a reset is a discontinuity
on the bus, not a long interrupts-off window.

## Register coverage

Everything lives in one 256-byte bank, filler `0x00`. `W` is
`METALCLAW_REG_WRITABLE`, `C` is `METALCLAW_REG_CLEAR_ON_READ`.

| reg | name | value | flags | why |
| --- | --- | --- | --- | --- |
| 0x00 | `CHIP_ID` | `0x50` | | the driver refuses anything else |
| 0x02 | `ERR_REG` | `0x00` | | the driver reads it last in init and fails on `conf_err` |
| 0x03 | `STATUS` | `0x70` | | `cmd_rdy`, `drdy_press`, `drdy_temp`; the fetch loop waits on `drdy_press` |
| 0x04..0x09 | `DATA_0..DATA_5` | the sample | | 24-bit pressure then 24-bit temperature, little-endian |
| 0x10 | `EVENT` | `0x01` | | `por_detected`, the part's reset value |
| 0x11 | `INT_STATUS` | `0x08` | C | `drdy`; nothing on this bench reads it |
| 0x15..0x18 | `FIFO_WTM_*`, `FIFO_CONFIG_*` | `0x01`/`0x00`/`0x02`/`0x02` | W | reset values, writable, otherwise inert |
| 0x19 | `INT_CTRL` | `0x02` | W | written only by the driver's trigger path, which this image does not build |
| 0x1a | `IF_CONF` | `0x00` | W | reset value |
| 0x1b | `PWR_CTRL` | `0x00` | W | the driver writes `0x33` at the end of init |
| 0x1c | `OSR` | `0x02` | W | reset value; the driver writes it unconditionally |
| 0x1d | `ODR` | `0x00` | W | the driver read-modify-writes it |
| 0x1f | `CONFIG` | `0x00` | W | IIR coefficient |
| 0x31..0x45 | `NVM_PAR_T1..P11` | the calibration below | | one 21-byte burst read at init |
| 0x7e | `CMD` | `0x00` | W | takes the write and does nothing; see below |

Everything else reads `0x00` and ignores writes, which is what a write to a
register with no `WRITABLE` flag does in `metalclaw_regmap_bus_write`.

**Soft reset is accepted and ignored.** The driver opens `bmp388_init()` with
`CMD = 0xB6` and then waits 2 ms. `CMD` is writable, so the byte lands and the
transaction completes normally, but nothing in the map changes: the chip id,
the calibration block and the sample all stay where they were. That is
deliberate. A real part would come back with its NVM reloaded and its config
registers at their reset values, which is what this map already holds at that
point in the sequence, and honouring the command would mean rebuilding the map
from interrupt context. The visible difference is that a soft reset issued
later — by a re-probe, or by hand — does not clear a configuration the DUT
wrote earlier. `reset()` from the harness is the way to get a fresh part.

## The Zephyr driver this is built against

Zephyr v4.4.0, `drivers/sensor/bosch/bmp388/`, on I2C, with
`CONFIG_BMP388_TRIGGER_NONE=y`. Its bus layer is `i2c_burst_read_dt` and
`i2c_reg_write_byte_dt`, which is exactly the register-pointer-then-burst
protocol `core/ports/rp2/hal/i2c_target.c` implements.

Its SPI backend, `bmp388_spi.c`, frames a read as the address with bit 7
set, one dummy byte, then the data, in one chip-select window, and a write as
address then value. It also refuses any read longer than six bytes while
`bmp388_init()` reads the 21-byte calibration block through it, so the
driver never comes up on SPI (v4.4.0, and still on `main`). The SPI image
therefore drives that same sequence from the rig, byte for byte, instead of
through the driver.

`bmp388_init()`, in order:

| step | access | what satisfies it |
| --- | --- | --- |
| `i2c_is_ready_dt` | no bus traffic | — |
| write `CMD` `0xB6` | 0x7e | writable, ignored |
| read `CHIPID` | 0x00 | `0x50` |
| read 21 bytes from `CALIB0` | 0x31..0x45 | the calibration block |
| read-modify-write `ODR`, mask `0x1F` | 0x1d | reads `0x00`, so the driver writes the devicetree `odr` index |
| write `OSR` | 0x1c | writable |
| write `CONFIG` | 0x1f | writable |
| write `PWR_CTRL` `0x33` | 0x1b | writable; `power_mode` then reads 3 |
| read `ERR_REG` | 0x02 | `0x00`, so no `conf_err` |

`bmp388_sample_fetch()` reads `STATUS` (0x03) until `drdy_press` is set — it is
always set here, so the loop runs once — then reads six bytes from `DATA_0`
(0x04). `bmp388_channel_get()` touches no register; it runs the Bosch
compensation over the burst and the calibration block.

The driver never reads `INT_STATUS`, `SENSORTIME`, `EVENT` or the FIFO in this
configuration, and it writes `INT_CTRL` only under `CONFIG_BMP388_TRIGGER`.

## The calibration block, and the maths it inverts

The published `NVM_PAR` values are chosen so that the driver's compensation
collapses to a plain scale factor. The block is what a 21-byte read of
0x31..0x45 returns:

```text
20 4e  00 40  00  ff 7f  00 40  00 00 00 00 00 00 00 00 00 00 00 00
t1     t2     t3  p1     p2     p3 p4 p5    p6    p7 p8 p9    p10 p11
```

| coefficient | value |
| --- | --- |
| `t1` | 20 000 |
| `t2` | 16 384 |
| `t3`, `p3`, `p4`, `p5`, `p6`, `p7`, `p8`, `p9`, `p10`, `p11` | 0 |
| `p1` | 32 767 |
| `p2` | 16 384 |

With `t3 = 0` and `t2 = 16384`, `bmp388_compensate_temp()` reduces to
`t_lin = raw_temp - 256 * t1`, and `bmp388_temp_channel_get()` reports
`t_lin * 250000 / 16384` microdegrees. With `p2 = 16384` and every other
pressure coefficient except `p1` zero, both `offset` and every temperature term
of `sensitivity` vanish, so `bmp388_compensate_press()` reduces to
`(p1 - 16384) * raw_press * 25 / 262144` hundredths of a pascal — **independent
of temperature**, which is what makes the two axes testable separately.

Inverting those, the model publishes:

```text
raw_press = (pressure_pa * 1000 * 262144 + D/2) / D,  D = (p1 - 16384) * 250 = 4 095 750
raw_temp  = t1 * 256 + (temperature_mc * 65536) / 1000
```

Both are integer expressions in `int64_t`, clamped to 24 bits. The pressure
form is the old implementation's, with its millipascal input reconstructed as
`pressure_pa * 1000`. One raw pressure LSB is 1/64.004 Pa ≈ 15.6 mPa and one
raw temperature LSB is 1/65.536 mC ≈ 15.3 µC, so the quantisation of the raw
words is far below the reported unit; both raw words stay inside 24 bits across
the whole property range (`0x1D4C75`..`0x7A13E8` and `0x262000`..`0xA32000`).

### Worst-case error, and the tolerance it sets

The error that survives is not the model's; it is the driver's own truncating
divides. `bmp388_press_channel_get()` floors hundredths of a pascal into
`val1`/`val2`, and one hundredth lost there is a whole pascal lost by the time
the DUT prints `pressure_pa`. Running the driver's exact integer arithmetic
over every pascal from 30 000 to 125 000 and every millidegree from -40 000 to
85 000 gives:

| axis | worst case | distribution |
| --- | --- | --- |
| pressure | 1 Pa | -1 Pa on 47 513 of 95 001 values, 0 on the rest; never +1 |
| temperature | 1 mC | -1 on 84 320, 0 on 1 001, +1 on 39 680 of 125 001 values |

**`devices/bmp388/test/cases.py` therefore asserts ±1 Pa and ±1 mC.** The
pressure bias is one-sided and could be removed by rounding the raw pressure up
instead of to nearest, which makes the round trip exact; the model keeps the
ported round-to-nearest so that the number on the wire is the one the old
implementation put there.

The harness's own `pressure` and `temperature` properties always read back
exactly what was set — they are the physical values, not the raw words.

## The DUT image

`test/dut/rp2.overlay` gives the Pico 2 an `i2c0` on GP4/GP5 — the fixture's
one I2C pair, with 2.2 kΩ pull-ups on the stack — and a `bmp388@76` node
aliased `barometer`. The pinctrl group is spelled out rather than inherited
from the board so the pin choice is reviewable here.

`odr = "50"` is not the default. The binding's default `"200"` is also the
chip's reset value, and `bmp388_reg_field_update()` skips a write whose value
is unchanged, so at the default the driver would never write `ODR` at all.
Asking for 50 Hz makes the read-modify-write on 0x1d real, which is the point
of binding a production driver to a simulated part; `odr` reading back 2 is the
assertion that it happened.

`test/dut/rp2.conf` adds `CONFIG_SENSOR`, `CONFIG_BMP388`,
`CONFIG_BMP388_TRIGGER_NONE` and `CONFIG_I2C` to the DUT fixture's `prj.conf`.
No interrupt line is wired on fixture 1, so there is no trigger and no
`int-gpios` property. No USB and no console: the DUT fixture answers through
the RAM mailbox.

```sh
just build-dut bmp388
METALCLAW_HARNESS_SERIAL=<serial> METALCLAW_ARM_TOOLCHAIN_DIR=<dir> \
METALCLAW_DUT_DEVICE=bmp388 just test hardware
```

`test/dut/spi/rp2.{overlay,conf}` is the SPI variant, image `bmp388/spi`:
`spi0` on GP18/GP19/GP16 with a GPIO chip select on GP22 and a
`vnd,spi-device` node aliased `raw` in mode 3 at 8 MHz. There is no
`bosch,bmp388` node because the driver cannot initialise over SPI (above);
the rig's `RAW` command clocks whatever bytes a case hands it in one window.

```sh
just build-dut bmp388/spi
METALCLAW_DUT_DEVICE=bmp388/spi just test hardware
```

## Tests

`test/cases.py` runs on the rig's `harness` and `dut` fixtures. It attaches the
model from the harness REPL on the bus the flashed image is built for, I2C at
`(4, 5, 0x76)` or SPI at `(18, 19, 16, 22)`, and then resets the DUT, because
Zephyr probes the driver once at `POST_KERNEL` and a part that was not armed
then never becomes ready. The cases set pressure and temperature, read the
part through the DUT, and compare `pressure_pa` and `temperature_mc` against
what was injected within the ±1 above; they walk both corners of both ranges,
check that a new sample reaches the DUT, that each fetch moves
`transactions`, that `power_mode`, `osr` and `odr` carry what the driver
wrote, and that a `reset()` clears that configuration and a DUT reset puts it
back.

The read goes through one of two readers. On the I2C image it is the DUT's
`SENSOR` command, the driver's own fetch and compensation. On the SPI image
the case clocks `bmp388_init()`'s sequence itself through `RAW` after every
DUT reset (soft reset, chip id, calibration block, `ODR` read-modify-write,
`OSR`, `CONFIG`, `PWR_CTRL`, `ERR_REG`), reads the six data bytes the same
way, and runs a port of the driver's integer compensation over them. The
eight cases are the same on both buses.

## Not modelled

The FIFO (0x12..0x18 hold their reset values but no data), `SENSORTIME`
(0x0c..0x0f read zero), the interrupt pin, `EVENT` and `INT_STATUS` semantics
beyond their reset values, oversampling and ODR actually affecting the sample,
and the conversion time a real part needs. Nothing the Zephyr driver does in
this configuration depends on any of them.

One instance costs 2 088 bytes of the runtime's 8 KiB arena, almost all of it
the register map's value and flag banks.
