<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# Devices and the runtime

A device is whatever the board under test finds on the other side of a pin,
and anything a user can name in `attach()`. Each one is a directory under
`devices/` that defines one `struct mclw_model` and nothing else the
runtime needs to know about. Fifty-nine exist:
[`swd_probe`](../devices/swd_probe/README.md), a CMSIS-DAP v2 debug probe;
[`console_bridge`](../devices/console_bridge/README.md), a USB serial port on
the board's console UART;
[`bmp388`](../devices/bmp388/README.md), a Bosch barometer on I2C or SPI;
[`ads7883`](../devices/ads7883/README.md), a Texas Instruments ADC on SPI;
[`icm4268x`](../devices/icm4268x/README.md), TDK InvenSense IMUs on SPI;
[`neo_m8n`](../devices/neo_m8n/README.md), a u-blox GNSS receiver on UART that
streams NMEA and speaks UBX;
[`sx126x`](../devices/sx126x/README.md), Semtech LoRa transceivers on
SPI with their BUSY, DIO1 and NRESET lines and a simulated air side;
[`cellular`](../devices/cellular/README.md), cellular modems on UART with
their PWRKEY or RESET_N and a small fake network behind them, one engine
shared by four models: [`sim7080g`](../devices/cellular/sim7080g.md) and
[`sim7000`](../devices/cellular/sim7000.md), SIMCom LTE-M/NB-IoT modems;
[`a7672x`](../devices/cellular/a7672x.md), a SIMCom LTE Cat-1 modem; and
[`sequans_monarch`](../devices/cellular/sequans_monarch.md), the Sequans
Monarch LTE-M/NB-IoT modems;
[`bmx280`](../devices/bmx280/README.md), Bosch BMP280 and BME280
environmental sensors on I2C or SPI, two models sharing the compensation
maths;
[`mcp9808`](../devices/mcp9808/README.md), a Microchip temperature sensor on
I2C with 16-bit registers;
[`pcf8523`](../devices/pcf8523/README.md), an NXP real-time clock on I2C
that counts on the simulation clock;
[`ina2xx`](../devices/ina2xx/README.md), Texas Instruments current and power
monitors on I2C, one engine shared by four models: `ina219`, `ina228`,
`ina237` and `ina260`;
[`mpu6050`](../devices/mpu6050/README.md), an InvenSense six-axis IMU on I2C;
[`lis3dh`](../devices/lis3dh/README.md), an ST accelerometer on I2C;
[`mma8451q`](../devices/mma8451q/README.md), an NXP accelerometer on I2C;
[`mpl3115a2`](../devices/mpl3115a2/README.md), an NXP pressure and altitude
sensor on I2C with its interrupt lines;
[`pa1010d`](../devices/pa1010d/README.md), a CDTop GNSS module that sends
NMEA and takes PMTK commands through its I2C buffer;
[`mpu9250`](../devices/mpu9250/README.md), an InvenSense accelerometer and
gyroscope on SPI or I2C;
[`w25q`](../devices/w25q/README.md), the Winbond W25Q16JV serial NOR flash on
SPI, as the model `w25q16jv`;
[`at45db`](../devices/at45db/README.md), Renesas' AT45DB041E DataFlash on SPI, as
the model `at45db041e`;
[`at24`](../devices/at24/README.md), the AT24C02C 2-Kbit I2C EEPROM, as the
model `at24c02`;
[`lps22hb`](../devices/lps22hb/README.md), ST's LPS22HB pressure sensor on
I2C;
[`lp3943`](../devices/lp3943/README.md), TI's LP3943 16-LED driver on I2C;
[`lp5562`](../devices/lp5562/README.md), TI's LP5562 RGBW LED driver on I2C;
[`is31fl3216a`](../devices/is31fl3216a/README.md), ISSI's IS31FL3216A
16-channel LED driver on I2C;
[`sx1509b`](../devices/sx1509b/README.md), Semtech's SX1509B I/O expander with
LED driver on I2C;
[`ft5336`](../devices/ft5336/README.md), FocalTech's FT5336 touch controller
on I2C;
[`lp50xx`](../devices/lp50xx/README.md), TI's LP5012 RGB LED driver on I2C, as
the model `lp5012`;
[`lp5569`](../devices/lp5569/README.md), TI's LP5569 9-channel LED driver on
I2C;
[`ad5693r`](../devices/ad5693r/README.md), Analog Devices' AD5693R 16-bit DAC
on I2C;
[`hts221`](../devices/hts221/README.md), ST's HTS221 humidity and temperature
sensor on I2C;
[`lsm303dlhc`](../devices/lsm303dlhc/README.md), the magnetometer of ST's
LSM303DLHC on I2C, as the model `lsm303dlhc_magn`;
[`drv2605`](../devices/drv2605/README.md), TI's DRV2605L haptic driver on I2C,
as the model `drv2605l`;
[`max6675`](../devices/max6675/README.md), Maxim's MAX6675 thermocouple
converter on SPI;
[`ds3231`](../devices/ds3231/README.md), Maxim's DS3231 real-time clock on
I2C with its INT/SQW pin;
[`zfm20`](../devices/zfm20/README.md), ZhianTec's ZFM-20 fingerprint module
on a UART;
[`mcp4728`](../devices/mcp4728/README.md), Microchip's MCP4728 quad 12-bit
DAC on I2C;
[`lsm6dso`](../devices/lsm6dso/README.md), ST's LSM6DSO and LSM6DSL 6-axis
IMUs on I2C with their INT1 pin, two models;
[`veml60xx`](../devices/veml60xx/README.md), Vishay's VEML6031X00 and
VEML6046X00 light sensors on I2C, two models;
[`pca9633`](../devices/pca9633/README.md), NXP's 4-bit I2C LED driver;
[`sht`](../devices/sht/README.md), Sensirion's SHT3x-DIS and SHT4x humidity
sensors on I2C, two models;
[`tmp1xx`](../devices/tmp1xx/README.md), Texas Instruments' TMP108, TMP112
and TMP116 temperature sensors on I2C, three models;
[`mcp2515`](../devices/mcp2515/README.md), Microchip's stand-alone CAN
controller on SPI with its INT pin, in its own loopback;
[`lcd_backpack`](../devices/lcd_backpack/README.md), Adafruit's serial
character LCD backpack on UART; five displays, each a sink:
[`ssd130x`](../devices/ssd130x/README.md), Solomon Systech SSD1306 and SSD1309
OLED controllers on I2C or 4-wire SPI, two models;
[`ht16k33`](../devices/ht16k33/README.md), a Holtek LED matrix driver on I2C;
[`hd44780`](../devices/hd44780/README.md), an HD44780 character LCD behind a
PCF8574 I2C backpack (`hd44780_pcf8574`) or an AIP31068L on I2C (`aip31068`);
[`max7219`](../devices/max7219/README.md), Maxim LED display drivers on SPI,
one or a cascade of up to eight;
[`st7789`](../devices/st7789/README.md), a Sitronix ST7789V TFT controller on
4-wire SPI (`st7789v`); and seven parts on wires whose timing they define:
[`ws2812b`](../devices/ws2812b/README.md), a strip of Worldsemi WS2812B
LEDs on their single-wire input;
[`apa102`](../devices/apa102/README.md), a strip of APA102C LEDs on clock
and data;
[`hub75`](../devices/hub75/README.md), a 128 x 64 HUB75 LED panel scanned
1/32;
[`ds18b20`](../devices/ds18b20/README.md), a Maxim thermometer on 1-Wire;
[`dht22`](../devices/dht22/README.md), an Aosong humidity and temperature
sensor on its single data line;
[`quadrature_encoder`](../devices/quadrature_encoder/README.md), an
incremental encoder the harness turns;
[`tsop382`](../devices/tsop382/README.md), a Vishay IR receiver module in
the path from an IR LED (`tsop38238`), which reads the NEC frames it sees;
and three for what a board finds on its pins beyond a part:
[`square_wave`](../devices/square_wave/README.md), a square wave on one pin:
a clock, a button pressed and let go, a wake signal, or, behind an RC
filter, a PWM level;
[`jumper`](../devices/jumper/README.md), a jumper wire between two of the
board's pins, push-pull one way (`jumper`) or open-drain both ways
(`jumper_open_drain`); and
[`spi_page`](../devices/spi_page/README.md), the other end of an SPI link
that moves 256-byte pages one byte per chip-select window, as the target
(`spi_page_target`) or the controller (`spi_page_controller`).

A device is written from its datasheet and contains no bus code. It declares
which buses the part speaks, fills an image the datasheet describes (a
register map, a frame, or a command image), and arms the engine that serves
it; or, for a part that only listens, arms a sink and decodes what the
engine captures; or, on UART, it parses what arrives and answers; or, on a
wire, it names a timing engine with the datasheet's times and reads what the
engine captured, or queues what it sends. The transports live in the port.

## The model

`core/include/metalclaw/model.h`. A model is static descriptor tables plus
an ops table:

- **properties**: name, type (`bool`, `i32`, `u32`), element count (up to 8),
  unit, min, max, access. Values travel as `struct mclw_value`; the
  runtime range-checks them before the model sees them.
- **regions**: named byte ranges (a register file, a FIFO) with a size and
  access.
- **faults**: names the model knows how to misbehave under.
- **bindings**: what the model needs from the platform, by name and kind.
  The names are the `attach()` keywords. `gpio_in` and `gpio_out` take a pin;
  `swd_wire` takes a tuple `(swclk, swdio[, nreset])`; `i2c_target` takes a
  tuple `(sda, scl, address)`; `spi_target` takes `(sck, mosi, miso, cs)`;
  `spi_sink`, a receive-only SPI target, takes `(sck, mosi, cs[, line])`,
  the line being the part's data/command select;
  `uart` takes `(tx, rx)`; `wire` takes as many pins as its facts say
  (below); `usb_interface` takes no keyword, the runtime
  allocates it from the descriptor the model carries (a vendor interface or
  a serial port, and its interface string) and delivers what the host sends
  to the model's `usb_rx` op, and a serial port's line coding and control
  line changes to its `usb_control` op.
  An `spi_target` binding carries the part's datasheet facts (the SPI modes
  it supports, the default, bit order) and a `uart` binding its default baud
  and format; the runtime opens the engine with them and `schema()` shows
  them. A part that supports several SPI modes exposes an `spi_mode`
  property; a UART part exposes `baud`. A `wire` binding carries its
  engine and the datasheet's times for it (`struct mclw_wire_facts` in
  `bus.h`); `schema()` shows the engine and the pin count.
- **state_size**: bytes of zeroed state the runtime hands the model.
- **ops**: `init` and `deinit` are required; `reset`, `get`, `set`,
  `inject_fault`, `clear_faults`, `read_region`, `write_region`,
  `time_advanced`, `poll`, `usb_rx`, `usb_control` are optional and report
  `ENOTSUP` when absent.
- **config**: the model's own constant data, for one source that defines
  several models (`icm4268x` picks its `WHO_AM_I` from it, `cellular` its
  command tables, identity and timing). The runtime never
  reads it.

A model owns its response image in its state, arms the bound engine from
`init`, and edits the image under `mclw_hal_critical_enter()` whenever
a property changes. The engine serves the image with no model call. A
callback that only holds the state gets its instance back with
`mclw_instance_from_state()`. The runtime has already refused a
missing required binding before `init` runs.

A model declares the ABI version it targets twice: in `device.cmake`, where
the generator refuses a mismatch at configure time, and in the struct, where
`attach()` refuses it at run time.

## Registration

A device directory contains `device.cmake` that calls

```cmake
metalclaw_device(bmp388 SYMBOL mclw_model_bmp388 ABI 1 SOURCES ${CMAKE_CURRENT_LIST_DIR}/bmp388.c)
```

`core/cmake/devices.cmake` globs every such file and generates the registry
table. Nothing else in the tree is edited to add a device.

## The runtime

`core/include/metalclaw/runtime.h`. Instances live in a static arena sized
at build time (`METALCLAW_ARENA_SIZE`, default 16 KiB, up to
`METALCLAW_INSTANCES_MAX`, default 8). `attach()` resolves each binding
through the HAL, allocates state, and calls the model's `init`. `detach()`
reverses it. Every instance is detached on a MicroPython soft reset.

The simulation clock starts at zero at boot and runs with real time: the
worker advances it once a millisecond and calls every model's
`time_advanced`, so a part that talks on its own keeps its cadence with no
script involved (`neo_m8n` sends a fix every interval). Between ticks a
running clock reads as the last tick plus the real time since, so
`mclw_now_us()` and trace stamps taken from a callback are exact to the
microsecond while `time_advanced` keeps its millisecond cadence. `pause()`
stops it;
`advance_us()` then steps it by exactly the amount asked, which is how a
case counts frames deterministically; `resume()` lets it run again from
where it is. On the same millisecond tick, paused or not, the worker calls
every model's `poll`: what the part decides follows the clock in
`time_advanced`, and what it already decided to send leaves in real time
from `poll`, as a real part's queued output drains at line rate whatever the
script does with the clock (`neo_m8n` feeds an epoch's NMEA to the UART
there, `cellular` its output FIFO and PPP frames). Each instance carries a
16-entry trace ring that models append to with `mclw_trace()`, stamped
with simulation time; a kind is one of `spi`, `i2c`, `uart`, `command`,
`state`, `error`. Every engine's counters reach a binding or a host
through one runtime call, `mclw_binding_stats()`, as named values.

## Where a model runs

Two cores. Core 0 runs MicroPython, bare metal, and TinyUSB. Core 1 is the
**worker**: in rounds it runs the work the engines post from their
interrupts (what was queued as the round began; work posted meanwhile
waits for the next), runs every registered poll, and ticks the clock, so
work that keeps coming never holds the polls off. Every engine interrupt
is enabled on core 1, so MicroPython's atomic sections never delay one.

| Model entry point | Runs on | Holds |
| --- | --- | --- |
| `get`, `set`, `reset`, regions, faults | core 0, the script's thread | the runtime lock |
| `usb_rx`, `usb_control` | core 0, the USB task | the runtime lock |
| `time_advanced`, `poll` | the worker | the runtime lock |
| `uart` `on_rx` | the worker | the runtime lock |
| `command` `on_window` | the worker | the runtime lock |
| `sink` `on_bytes` | the worker | the runtime lock |
| `regmap` `on_write`, `stream` `on_frame` | an engine interrupt | the critical section |
| `onewire` network layer (core, per slot) | an engine interrupt | the critical section |

The runtime lock (`mclw_hal_lock`) is what lets a model touch its
state from `set` and from `on_rx` without further care. Image edits from
any context go under `mclw_hal_critical_enter()`. It makes a multi-byte
update atomic with respect to interrupt-context code such as `on_write` and
to each per-byte call of the I2C engine; the I2C engine leaves it between
bytes and the SPI engine's DMA reads the live image, so a burst that
overlaps an update can mix the old image and the new one. Both are in `hal/platform.h`, with
real time (`mclw_hal_time_us`, `mclw_hal_delay_us`), the work
queue and the poll table.

## Images

The response images are what a model edits and an engine serves.

`core/include/metalclaw/regmap.h` is a register file with up to four banks,
per-register flags for writable and clear-on-read, multi-byte setters in
either byte order, and bus-side read/write helpers an engine calls without
the model. A model may install an
`on_write` hook: it runs in interrupt context after a write has been
committed, bank select included, and may only edit the image. The hook,
and whatever it calls, is defined with `MCLW_RAM_FUNC` so it runs from RAM,
as `icm4268x`'s is; the regmap's own functions already are. For SPI the
model adds a `struct mclw_regmap_framing` (in `hal/spi_target.h`) from
the datasheet: which command bit means read, which bits are the address,
whether a multi-byte access walks consecutive addresses, and whether one
dummy byte precedes read data. The same map serves I2C and SPI; `bmp388`
binds either. A part whose register pointer does not name one byte (registers
wider than a byte, an auto-increment flag in the pointer, a FIFO behind an
alias) gives the map a pointer map: for each pointer value, the image
address a transfer starts at. The I2C engine applies it and then runs on
through the image one byte per bus byte. With a pointer map, every
transaction starts again at the image address its last pointer byte
selected, so a read with no pointer write re-reads the selected register
from its first byte; arming the map selects pointer value 0. That is how
the pointer registers of TI's INA2xx (SBOS448G 8.5.6, SLYS021A 7.5.1.1) and
Microchip's MCP9808 (DS25095A 4.1.1) behave. Without one, the pointer is a
byte address that runs on from one transaction to the next. A part with no
register pointer at all, a byte stream behind its address, gives the map an
`on_input` hook instead: every transaction starts at image address 0 and
every byte the master writes goes to the hook, in interrupt context, rather
than into the image. That is the I2C buffer of MediaTek's GNSS receivers
(GlobalTop's "NMEA over I2C" guide, 3 and 4), which `pa1010d` serves.

`core/include/metalclaw/stream.h` is the other image: a fixed-length frame
of up to 64 bytes, double-buffered, that the part drives out (`OUT`, an ADC)
or receives (`IN`, delivered through `on_frame` after the window closes).
`mclw_stream_publish()` swaps in a new frame between windows.

`core/include/metalclaw/command.h` is for parts whose SPI windows are framed
by their first byte, the opcode, and that clock a status byte out while the
host sends the header (Semtech SX126x and its kin). The image holds the
status byte, up to eight fixed responses for read opcodes, a register space
of up to seven 256-byte pages addressed by 16 bits, and a 256-byte buffer
read from an 8-bit offset that wraps. Every window is answered from
hardware; afterwards the engine hands what the host sent to the image's
`on_window` hook on the worker, under the runtime lock, where the model
executes the command, edits the image and calls
`mclw_command_publish()`. A part like this holds a BUSY line high
until it has processed a command, and the host waits for it, which is what
makes all of that free of timing: the engine raises BUSY in hardware at
every chip-select release and the model lowers it with
`mclw_hal_spi_target_set_busy()` once its edits are published.

The same image serves a serial NOR flash. Its Read Data is a memory read:
the opcode, three address bytes and the array from that address, with no
NOP, served from the image's pages by address bits 15..8 and 7..0; bits
23..16 reach the model and select nothing. A page the image does not hold
reads as the image's filler (`mclw_command_set_filler`, 0xFF for a flash).
Such a part has no BUSY line: it reports its own busy state in the status
byte and the host polls that, back to back. The image names the opcodes
that start an internal operation and the status bits that say so; the
engine sets those bits at the release of such a window and holds them until
the model has taken it, so a poll right after a program or erase never sees
the part idle too early. Without a BUSY line the engine queues up to eight
windows for the worker rather than one, so a write enable and the erase
right behind it both reach the model; a host polling faster than the
worker delivers fills that queue with polls, and each new window then
displaces a queued one that only repeats the window before it. A publish
of the status reaches the next window from the engine at once, and one of
the rest lands at once even while a window that reads only the status byte
is open. `w25q` is built this way. An image may instead have one immediate
response, which follows the opcode with no NOP (a NOR flash's Read JEDEC
ID); the engine then serves every window's second byte once the opcode is
decoded, not ahead of it, and takes no BUSY line with it.

`core/include/metalclaw/sink.h` is for parts that only listen and have no
register map to serve: display controllers, LED drivers, a port expander.
Their bytes are commands and data whose meaning depends on what came
before, so the engine serves nothing and captures everything: each byte
the host writes, flagged when it is the first of a window (after chip
select fell, or after START and the address) and, on SPI, with the level
of the part's data/command line as its last bit was clocked. The engine
hands them to the sink's `on_bytes` on the worker under the runtime lock, a
batch at a time, with whether the window they end has closed. The model
decodes them there, so its state needs no critical section: the host reads
it under the same lock. An I2C read of a sink returns the one byte the
model leaves in `read_byte`, so a part with no register pointer whose reads
carry data, as `pa1010d`'s NMEA buffer does, takes a regmap with an
`on_input` hook instead. An I2C target holds one image or one sink; arming
either replaces the other.

`core/include/metalclaw/onewire.h` is a 1-Wire slave's image: its ROM
code, a small memory, and a table of function commands, each answered
from the image (bytes sent from the memory, bytes received, read slots
that return a status bit). The network layer in `core/src/onewire.c`
answers reset, the ROM commands (search, read, match, skip, alarm search)
and those function commands, one call a slot from the engine's interrupt
under the critical section; it returns what the part does in the next
slot. The model takes the commands issued, and what a write command
received, from its poll, and edits the memory and the status under the
critical section. A status change shows a slot late at most.

## The HAL

`core/include/metalclaw/hal/`. Ports implement it; nothing above it knows a
port exists.

- `platform.h`: the execution model: the critical section, the runtime
  lock, real time, the work queue and poll table of the worker, and
  `MCLW_RAM_FUNC`, which places a function on an engine interrupt's path
  in RAM.
- `pins.h`: pin ownership. Claims are per owner; a board may name pins that
  no device may ever claim with `METALCLAW_RESERVED_PINS`.
- `gpio.h`: one pin driven or sampled by a device. An input also latches a
  fall, so a pulse that comes and goes between two reads is still seen. An
  output can carry a square wave from the port's hardware (on rp2 a PWM
  slice with an integer divider: 16 ns to about 134 ms at 125 MHz; both
  pins of a slice share its period), and two pins can be linked in
  hardware: one output following an input a few nanoseconds behind it, or
  two pins joined as one open-drain line. On rp2 a link is one PIO state
  machine with no CPU or DMA (`mov pins, pins`, one instruction; the
  open-drain join, twelve, waits out a 2 µs settle after it lets a side go
  before it looks at that side again).
- `spi_peer.h`: an SPI peer on four pins a model holds as gpio bindings,
  mode 0, MSB first, one byte per chip-select window, the framing a PL022
  uses in mode 0: a target that shifts out the next byte of a 256-byte
  ring per window and keeps what came in at the same place, or a controller
  that clocks a transfer at its own rate. On rp2 one PIO state machine
  (nine and seven instructions) and two DMA channels each.
- `swd.h`: the SWD wire: transfers, bit sequences, pin control, clock. On
  rp2 it is a PIO program; every wait is bounded and a timeout resyncs the
  state machine. A wait for the state machine's FIFOs sleeps on WFE, woken
  by the FIFO level on one of the block's interrupt lines that no handler
  is installed on; the interrupt itself stays disabled.
- `i2c_target.h`: an I2C target that serves a `regmap` from hardware. The
  model arms it once and updates the image under a critical section; the
  engine answers the bus from interrupt context without the model. The
  first byte the master writes is the register pointer; with the image's
  pointer map, each transaction starts again at the register it selected;
  with its `on_input` hook there is no pointer and written bytes go to the
  hook. Armed with a sink instead of an image, it captures every written
  byte into a 256-entry ring from the interrupt, and the worker's poll
  delivers them. Counters: `transactions`, `bytes`, `dropped` (sink bytes
  the worker had no room for).
- `spi_target.h`: an SPI target that serves a `regmap` (with its framing),
  a `stream` or a `command` image from hardware, in the mode the model
  asked for, with `set_mode` to change it. It publishes a clock ceiling per
  image (10.7 MHz, 15 MHz and 8 MHz; 2 MHz for a command image with a
  memory read, an estimate not yet measured on the bench: the first data
  byte must reach the state machine within one SCK period of the last
  address bit, and the chain spends about 200 ns there; 1 MHz for one
  with an immediate response, whose second byte waits on about five blocks
  of the chain) and counters (`transactions`, `bytes`,
  short and long windows, DMA faults, stale windows; for a command image a
  stale window is one that began while BUSY was high or that found the
  worker's queue full; a missed window is one the engine served but could
  not hand to the model). On rp2 it is one PIO program and five DMA
  channels per target, and no CPU runs at a chip-select edge: at every rise
  a reset program of DMA control blocks, started by the chip-select monitor
  state machine, restarts the data state machine for the next window,
  records the window that ended in one of four slots and arms the response
  program again; a DMA interrupt then hands the window to the model, through
  `on_write` or `on_frame` there, or through `on_window` on the worker.
  Windows reach the model in order however close together they come. With
  no window open, a changed stream frame is armed by running the reset
  program again, and a register map's changed bank, or a command image's
  changed status if it has BUSY, is put in the next window by the CPU in a
  few instructions, which cuts a window that a host ignoring BUSY begins in
  those instructions; an image without BUSY has its status from the next
  release. A register map's command byte and a command image's opcode, offset and
  address lookups are chains of DMA control blocks that patch each other,
  which is what bounds their clocks. Two targets may share one bus; each
  keeps its own chip select. Measured on the carrier's 150 MHz RP2350
  harness from the reset program's first block: the data state machine runs
  again 0.25 µs after it (36 to 38 cycles) for a command image or a
  register map and 0.46 µs (68 to 72) for a stream, and one DMA transfer
  later for the forced jump that first parks it, not measured, so a window
  may begin clocking that soon after the rise before it, and the response
  program is armed 1.9 µs after it for a register map and 2.7 µs for a
  command image, so a window's first byte must take at least that long. A rise that comes
  while the reset program for the one before it still runs, under about
  3 µs apart, is re-armed for only once that one ends. Not measured on
  fixture 1's 125 MHz RP2040; scaled by clock, about 0.3 µs, 0.55 µs and
  3.2 µs.
- `spi_sink.h`: a receive-only SPI target for a sink, on `(sck, mosi,
  cs[, line])`. Chip select may rise after every byte, as a PL022 does in
  Motorola mode with CPHA 0, or stay low for a whole frame: the CPU
  re-arms nothing between windows. On rp2 it is two state machines (20
  instruction words) and two DMA channels: one state machine shifts each byte in with a window
  flag and the line's level; at every chip-select rise the other has a DMA
  channel write a jump into the first's instruction register, which drops
  a partial byte and waits for the next fall, so SCK edges while chip
  select is high (a DUT resetting on a bench with line pull-ups) never
  count as bits; and a DMA channel streams the words into a 2048-entry
  ring that the worker's poll reads. Its clock
  ceiling, 10 MHz, is worked out from the programs' instruction counts, not
  measured. Counters: `bytes`, `windows`, `overruns`, `max_clock_hz`. One
  sink at a time.
- `uart.h`: a UART with `send()` that queues and returns, `set_baud`,
  `set_format`, and a receive callback delivered on the worker under the
  runtime lock with whatever accumulated, never per byte and never from an
  interrupt, so a model parses, touches its state and answers from inside it.
  On rp2 any pin pair works: a pair that carries a PL011's TX and RX
  functions gets that block while it is free; any other pair gets a PIO
  engine: a transmit and a receive state machine, 14 instruction words
  shared by every PIO UART on that block, DMA both
  ways, 8 data bits, no parity, one or two stop bits, two at once at most.
  Its receive ring holds 1 KiB, about 90 ms at 115200 baud. Counters:
  `bytes_rx`, `bytes_tx`, `frame_errors` and `overruns`, the received bytes
  lost, split into `fifo_overruns` (the receiver's FIFO was full: a PL011's
  overrun flag, a PIO receiver stalled) and `ring_overruns` (the port's
  receive ring was), and `baud`.
- `wire.h`: a few pins whose timing a part defines, run by one of the
  port's wire engines, which the binding's facts name: `pulse_bits_in` (a
  single-wire LED line: each high pulse a bit by its width, frames ended by
  a long low), `clocked_in` (data sampled at each rising clock edge),
  `latched_in` (a shift-register panel, looked at one latch at a time:
  every pin in reach sampled at each clock edge and at the latch, and the
  enable line's time timed, from a PIO block that reaches the `pins`
  bindings its model declares before it), `pins` (pins another binding's
  engine samples), `onewire` (a 1-Wire slave's reset, presence and slots, with the
  network layer above), `responder` (an open-drain line that answers a
  host's start pulse with a waveform), `steps_out` (consecutive pins stepped
  through queued states, one each period) and `envelope` (an IR receiver's
  output following a carrier, with each burst and gap timed). A model reads
  what an input engine captured, or queues what an output engine sends,
  from its poll. On rp2 each engine is a PIO program encoded at open from
  the facts and the system clock, state machines with identical programs
  sharing one copy:
  `pulse_bits_in` 26 instruction words, `clocked_in` 3, `latched_in` 30 on
  two state machines, `onewire` 27, `responder` 15, `steps_out` 3,
  `envelope` 14. Input engines but `envelope` stream through a DMA channel
  into 2 KiB blocks of an 8 KiB static pool, four blocks, which the
  `latched_in` and `responder` engines also use; `onewire` has a DMA channel
  whose completion interrupt, on DMA_IRQ_1 on the worker core, runs the
  network layer. Edges are timed to two harness cycles or so. Counters:
  `words`, `frames`, `errors`, `overruns`.
- `usb.h`: a function on the composite device, a vendor bulk interface or
  a CDC ACM serial port, with a receive callback that says how much it
  took, a control callback for a serial port's line coding and lines, and
  a write; see [usb.md](usb.md).
- `console.h`: the channel the harness's own REPL runs over; the port owns
  the transport.

On rp2 the SPI target engine takes PIO1 alone, all 32 of its instruction
words, two state machines and five DMA channels a target. Every other PIO
engine (the SWD probe, the PIO UART, the SPI sink, the SPI peer, gpio
links and the wire engines) tries PIO0, then PIO2
where the chip has one (the RP2354B; the RP2040 has two blocks), then
PIO1, and takes the first with room and a GPIO window that reaches its
pins. The probe is attached for every rig run and takes 11 words of PIO0,
which leaves 21: `pulse_bits_in` (26), `onewire` (27) and `latched_in`
(30) never fit beside it, so on an RP2040 harness they take PIO1 and
exclude an SPI target part, and on an RP2354B they take PIO2. DMA_IRQ_1 is
shared by the SPI target and the `onewire` engine, each a shared handler.

Engines that look alike serve different framings and are kept apart. The
SPI sink frames bytes by chip select and samples a data/command line;
`clocked_in` has no chip select and delivers 32-bit words from the first
clock, as an APA102's start frame needs. The SPI peer moves one byte per
chip-select window from a ring, which a PL022 in mode 0 needs and the SPI
target, which takes a window's first byte as a command and re-arms after
it, cannot serve. A gpio link relays a level between two DUT pins;
`envelope` drives its output from a carrier it demodulates and times.

On rp2 the worker is core 1, launched before the first `mp_init` with its
own stack and never reset; a soft reset leaves it running. It parks itself
for MicroPython's flash writes through the SDK's lockout, so a flash write
delays engine interrupts for its duration, as it always did on one core.

## From MicroPython

```python
import metalclaw
metalclaw.models()                       # ids of every model in the build
metalclaw.schema("bmp388")               # properties, regions, faults, bindings
metalclaw.board()                        # what the build knows of its board: {"line_pullups": False}
d = metalclaw.attach("bmp388", "baro", i2c=(4, 5, 0x76))
d = metalclaw.attach("bmp388", "baro", spi=(18, 19, 16, 22))
g = metalclaw.attach("neo_m8n", "gnss", uart=(0, 1))
d.get("pressure"); d.set(pressure=101325); d.pressure = 101325
d.stats("spi"); g.stats("uart")          # the engine's counters, by binding name
d.schema(); d.reset(); d.detach()
d.trace()                                # (t_us, kind, addr, len, result) tuples; kind is a name
metalclaw.instances(); metalclaw.detach("baro")
metalclaw.now_us(); metalclaw.running()          # the clock runs from boot
metalclaw.pause(); metalclaw.advance_us(1000); metalclaw.resume()

d.faults(); d.inject(name, 3); d.clear_faults()   # a model that declares faults
d.read_region(name, 0, 16); d.write_region(name, 0, b"\x00")   # ... or regions
```

Property names are attributes on the instance. Unknown names raise
`ValueError`, out-of-range values raise `ValueError`, and platform refusals
raise `OSError` with the errno. `sx126x` is the one device that declares
faults and regions: its regions carry the frames that go to and come from
the air, and its faults corrupt the next injected frames. Every other
device answers `ENOTSUP` to both sets of calls.
