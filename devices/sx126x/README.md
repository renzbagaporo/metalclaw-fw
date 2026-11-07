<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# sx126x

Semtech SX126x sub-GHz LoRa transceivers on SPI, with the part's BUSY,
DIO1 and NRESET lines and a simulated air side. Four models come out of one
file, differing only in the version string at register `0x0320` and the
over-current reset value:

| model | version string | OCP reset |
| --- | --- | --- |
| `sx1262` | `SX1261 V2D 2D02` (an SX1262 reports itself as an SX1261; RadioLib matches on that) | `0x38` |
| `sx1261` | `SX1261 V2D 2D02` | `0x18` |
| `sx1268` | `SX1268 V2D 2D02` | `0x38` |
| `llcc68` | `LLCC68 V2D 2D02` | `0x38` |

The board under test runs its own driver (Zephyr's loramac-node or native
driver, RadioLib, Meshtastic) against the model. Everything the driver
transmits is captured with its air parameters; the host puts frames on the
air for the driver to receive.

The model is one file with no bus code. It fills a
`struct mclw_command_image` (`core/include/metalclaw/command.h`) and
arms the SPI target engine with it: every window is answered from hardware,
and the model sees each one afterwards, on the worker, as the opcode and
what followed. The engine raises BUSY at every chip-select release; the
model executes the command, publishes the edited image, and lowers BUSY.
The host waits for BUSY before every command, so nothing the model does
between windows is timing-critical.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `spi` | `spi_target` | yes | an SPI target, from `(sck, mosi, miso, cs)`; mode 0, MSB first |
| 1 | `busy` | `gpio_out` | yes | BUSY; the engine drives it while the image is armed |
| 2 | `dio1` | `gpio_out` | no | DIO1, high while an IRQ in the DIO1 mask is set |
| 3 | `nreset` | `gpio_in` | no | NRESET; low holds the part in reset |

The engine raises BUSY from the SPI engine's PIO block when the chip
select rises, so BUSY must be a pin that block reaches: on an RP2350B
harness, the same half of GPIO0-31 or GPIO16-47 as the bus.

On fixture 1 the radio sits on SPI0 with its chip select on GP21, BUSY on
GP10, DIO1 on GP20 and NRESET on GP6, the same GP numbers on both boards:

```python
import metalclaw
radio = metalclaw.attach("sx1262", "radio", spi=(18, 19, 16, 21), busy=10, dio1=20, nreset=6)
```

On the MetalClaw Pico RP2 carrier the same Pico pins are reached through
the carrier's GPIOs:

```python
radio = metalclaw.attach("sx1262", "radio", spi=(38, 32, 35, 41), busy=22, dio1=40, nreset=14)
```

Without `nreset` the part never resets from its pin; bind it whenever the
DUT drives the line, and leave it unbound when the DUT leaves it floating.

## The air side

Every `SetTx` captures the payload and the parameters it went out with,
into a queue of four frames; a fifth is counted in `tx_dropped` and lost.
The oldest frame is the `tx` region and the `tx_*` properties;
`tx_next = True` drops it and shows the next. `tx_*` and the region raise
`OSError(ENOENT)` with the queue empty.

```python
TX = ("length", "frequency", "packet_type", "sf", "bw", "cr", "ldro", "preamble",
      "implicit_header", "crc", "iq_inverted", "sync_word", "power", "start_ms", "airtime_us")
frames = []
while radio.tx_pending:
    frames.append((radio.read_region("tx", 0, radio.tx_length),
                   {k: radio.get("tx_" + k) for k in TX}))
    radio.tx_next = True
```

Writing the `rx` region puts a frame on the air now. The `rx_*` properties
set this one frame's parameters and its signal; any that are not set are
the radio's own (the frame is on the radio's channel), RSSI -60 dBm, SNR
10 dB (`rx_snr` counts centidecibels, 1000), signal RSSI equal to the RSSI. They return to that once the frame is
on the air. Faults corrupt the next frames: `radio.inject("crc_error")`
makes the next injected frame fail its CRC, `"header_error"` its header.

```python
radio.set(rx_rssi=-73, rx_snr=725)                     # SNR 7.25 dB
radio.write_region("rx", 0, b"hello")                   # received if the radio listens

radio.set(rx_frequency=869525000, rx_sf=10)
radio.write_region("rx", 0, b"elsewhere")               # counted in rx_missed

radio.inject("crc_error")
radio.write_region("rx", 0, b"garbled")                 # RxDone | CrcErr
```

A frame is received when the radio is in RX or RX duty cycle at the moment
it starts, or enters one while at least 8 symbols of the frame's preamble
are still to come, as a real SX1262 does, with the packet type LoRa, the
same spreading factor, bandwidth, sync word and IQ polarity, and a carrier
within a quarter of the bandwidth. Otherwise it is counted in `rx_missed`;
it is still on the air for its air time, which is what channel activity
detection sees. A frame the radio stops receiving before RxDone (a command
that leaves RX, the RX timeout, a reset) is counted in `rx_missed` too.

### Timing

Air time is section 6.1.4 of the datasheet, rounded as RadioLib's
`getTimeOnAir` rounds it (`tx_airtime_us`). Events run on the simulation
clock and are checked on its millisecond tick, so each lands within a tick
after its time:

| event | when |
| --- | --- |
| TxDone | the air time after `SetTx`; the radio then goes to its fallback mode |
| Timeout | the `SetTx` or single `SetRx` timeout, in steps of 15.625 µs |
| PreambleDetected | 8 symbols into the frame (fewer if the preamble is shorter), or 8 symbols after a receiver that joined late started listening |
| HeaderValid, HeaderErr | after the preamble, sync word and the 8-symbol header block |
| RxDone (and CrcErr) | the frame's air time after it started; the payload is in the buffer at the RX base address |
| CadDone (and CadDetected) | the `SetCadParams` symbol count after `SetCad`; detected if a frame on the radio's channel overlaps it |

A single `SetRx` stops its timeout timer at the header, or at the preamble
after `StopTimerOnPreamble`. A frame received in single or duty-cycle RX
returns the radio to its fallback mode; continuous RX stays. A paused clock
pauses all of it.

`GetPacketStatus` reports the frame's RSSI and signal RSSI as `-2 × dBm`
and its SNR as `4 × dB`, rounded to the part's quarter decibel, the
encodings the drivers decode. `GetRssiInst`
reports the frame's RSSI while one is on the air and `noise_floor`
otherwise.

## Properties

The radio as the DUT left it, read the LoRa way:

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `mode` | u32 | ro | | 0 sleep, 1 STDBY_RC, 2 STDBY_XOSC, 3 FS, 4 TX, 5 RX, 6 RX duty cycle, 7 CAD, 8 held in reset |
| `packet_type` | u32 | ro | | 0 GFSK, 1 LoRa |
| `frequency` | u32 | ro | Hz | `SetRfFrequency`, converted |
| `sf`, `bw`, `cr`, `ldro` | u32, u32 (Hz), u32, bool | ro | | `SetModulationParams`; `cr` 1 to 4 is 4/5 to 4/8 |
| `preamble`, `implicit_header`, `payload_length`, `crc`, `iq_inverted` | | ro | | `SetPacketParams` |
| `sync_word` | u32 | ro | | registers `0x0740..0x0741`: `0x1424` private, `0x3444` public |
| `power` | i32 | ro | dBm | the output power `SetPaConfig` and `SetTxParams` give (below) |
| `irq` | u32 | ro | | the IRQ status |
| `dio2_rf_switch` | bool | ro | | `SetDio2AsRfSwitchCtrl` |
| `tcxo_mv` | u32 | ro | mV | `SetDio3AsTcxoCtrl`, 0 when not set |
| `commands`, `command_errors` | u32 | ro | | commands executed, and refused |
| `resets` | u32 | ro | | times NRESET pulled the part into reset |
| `spi_mode` | u32 | rw | | always 0 |
| `noise_floor` | i32 | rw | dBm | what `GetRssiInst` reports with nothing on the air, default -120 |

The frames:

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `tx_pending`, `tx_total`, `tx_dropped` | u32 | ro | | frames in the queue, sent, lost to a full queue |
| `tx_length`, `tx_frequency`, `tx_packet_type`, `tx_sf`, `tx_bw`, `tx_cr`, `tx_ldro`, `tx_preamble`, `tx_implicit_header`, `tx_crc`, `tx_iq_inverted`, `tx_sync_word`, `tx_power` | | ro | | the oldest frame, as the radio was set when it went out |
| `tx_start_ms` | u32 | ro | ms | simulation time of its `SetTx` |
| `tx_airtime_us` | u32 | ro | µs | its air time |
| `tx_next` | bool | wo | | `True` drops the oldest frame |
| `rx_received`, `rx_missed` | u32 | ro | | frames delivered with RxDone (CRC errors included), and not heard |
| `rx_frequency`, `rx_sf`, `rx_bw`, `rx_cr`, `rx_preamble`, `rx_crc`, `rx_iq_inverted`, `rx_sync_word` | | rw | | the next injected frame; reads show what it will carry |
| `rx_rssi`, `rx_snr`, `rx_signal_rssi` | i32 | rw | dBm, cdB, dBm | its signal |

`power` is the RadioLib measurement for the PA settings when they are one
of the 32 pairs RadioLib uses (`paOptTable` in its `SX1262.cpp`, from
radiolib-org/power-tests), and the `SetTxParams` value otherwise, which is
what Semtech's full-duty settings (`04 07 00 01`) give.

Regions: `tx` (255 bytes, ro), `rx` (255 bytes, wo, written at offset 0).
Faults: `crc_error`, `header_error`. The trace records each command as
`command` (opcode, window length, 0 or -EINVAL when refused) and each IRQ
raised as `state` (the bits).

`reset()` is a power-on reset and empties the frame queue; `factory` also
restores `noise_floor` and clears pending faults.

## Commands and status

Every command the drivers send is executed: standby, sleep (warm and cold,
woken by any chip-select pulse), FS, TX, RX (single, timed, continuous), RX
duty cycle, CAD and its parameters, packet type, frequency, modulation and
packet parameters, PA configuration and TX parameters, buffer base
addresses, DIO and IRQ parameters, IRQ status and clear, device errors,
statistics, RX buffer and packet status, instantaneous RSSI, DIO2 as RF
switch, DIO3 as TCXO control, calibration, image calibration, regulator
mode, fallback mode, StopTimerOnPreamble, LoRa symbol timeout, register and
buffer reads and writes. `0x00` is ResetStats; clocked alone, as RadioLib
does to wake the part, it is ignored.

The status byte is `0x80 | chip mode << 4 | command status << 1` on every
byte of a window that carries no data: bit 7 is reserved in the datasheet
and reads 1 on a real SX1262. The GetStatus response is the same byte
without bit 7. On GetStatus's second byte the part clocks that response and
the engine the status byte, the one place the two differ.

Out of reset `GetDeviceErrors` reports `XOSC_START_ERR` (`0x0020`), as a
module whose TCXO is powered from DIO3 does before `SetDio3AsTcxoCtrl`;
`ClearDeviceErrors` clears it.

The command status is 5 ("failure to execute") out of reset until the first
command, 1 after a command was executed, 4 after one was refused (unknown
opcode, missing parameters, a value out of range), 6 after TxDone and 2
after RxDone. Reads (every Get command, ReadRegister, ReadBuffer) and
ClearIrqStatus leave it as it was, so "data available" and "TX done" stay
until the next command that does something. It reaches the host on its
next window, as on the part. A window that starts while BUSY is high is
served from the image as it was but not executed, except that any window
wakes a sleeping part. Asleep or with NRESET low the part answers `0xFF`
on every status and response byte, and NRESET low holds BUSY high.

## Register coverage

Seven pages are held: `0x02`, `0x03`, `0x05`, `0x06`, `0x07`, `0x08`,
`0x09`. Everything in them is writable and reads back, except the version
string (`0x0320..0x032F`) and the random number registers
(`0x0819..0x081C`, which change after each read). Other pages read as zero.
Reset values:

| register | value | |
| --- | --- | --- |
| `0x029F` | `0x00` | retention list count; loramac-node reads the list and must find room in it |
| `0x0320..0x032F` | the version string | RadioLib's `begin()` checks it |
| `0x06B8..0x06C7` | whitening, CRC seed and polynomial, GFSK sync word | datasheet reset values |
| `0x0736` | `0x0D` | IQ polarity setup, read-modified-written by both drivers |
| `0x0740..0x0741` | `0x14 0x24` | LoRa sync word, private |
| `0x0749`, `0x076B..0x076D` | `0x00` | the last explicit header received: coding rate in `0x0749` bits 6:4, CRC on in `0x076B` bit 4, `0x076B` bit 5 set as on the part, no frequency error |
| `0x0889` | `0x04` | TX modulation workaround |
| `0x08AC` | `0x94` | RX gain |
| `0x08B5` | `0x04` | undocumented; Meshtastic sets bit 0 for its RX fix and every `Calibrate` clears it, as on the part |
| `0x08D8` | `0xC8` | TX clamp workaround |
| `0x08E7` | per model | over-current protection; `SetPaConfig` sets it for the PA chosen |
| `0x0911..0x0912` | `0x05` | XTA/XTB trim; `SetDio3AsTcxoCtrl` sets `0x2F` |

## The engine

`hal/spi_target.h`'s command image, served on rp2 by the stream state
machine and a chain of DMA control blocks that look up the opcode, the
buffer offset and the register address without the CPU. Its qualified
clock is 8 MHz: a response read has one byte time from its opcode to its
first data byte, and the chain needs about 600 ns of it. RadioLib's
default is 2 MHz and Meshtastic's 4 MHz.

## The DUT images

| image | driver |
| --- | --- |
| `sx126x` | Zephyr v4.4.0's loramac-node backend (`CONFIG_LORA_MODULE_BACKEND_LORAMAC_NODE`) |
| `sx126x/native` | Zephyr v4.4.0's native backend |
| `sx126x/raw` | none: the radio as a `vnd,spi-device` on the `raw` alias, for byte-level cases |

The driver images put a `semtech,sx1262` node aliased `lora0` on `spi0`
(GP18/GP19/GP16) with a GPIO chip select on GP21, `reset-gpios` GP6,
`busy-gpios` GP10, `dio1-gpios` GP20, DIO2 as the RF switch and a 1.8 V TCXO
on DIO3, at 8 MHz. The loramac-node backend needs the `loramac-node`
module, which `test/rig/dut/west.yml` lists.

```sh
just build-dut sx126x
just build-dut sx126x/native
just build-dut sx126x/raw
METALCLAW_HARNESS_SERIAL=<serial> METALCLAW_ARM_TOOLCHAIN_DIR=<dir> \
METALCLAW_DUT_DEVICE=sx126x just test hardware
```

## Tests

`test/cases.py`. On either driver image: the driver brings the part up
(reset seen, no command refused, TCXO and RF switch set, left asleep); two
configurations land in the model; payloads of 5, 40 and 255 bytes are
captured exactly with every air parameter, and TxDone follows `SetTx` by
the computed air time on the harness's clock; injected frames are received
with their RSSI and SNR; a frame on another frequency, spreading factor,
bandwidth, sync word or IQ polarity is not received and is counted as
missed; a receive with nothing on the air times out; a frame with a CRC
fault ends in `EIO`; the public sync word selects which frames are heard;
no window faulted. On the raw image: the version string, the status byte
on the header and after power-on, the wake-up NOP leaving it, refused
commands showing in it until the next command, register
write-back, the buffer wrapping at 256, TxDone and its command status, the
receive IRQs in order with RX buffer and packet status, a single receive
timing out, CAD with and without a frame on the air, a duty-cycled receiver
hearing a frame, and a thousand windows of each read framing per clock at
1, 2, 4 and 8 MHz with no mismatch and no DMA fault.

`test/traces/` holds two SPI traces of a real SX1262 under RadioLib 7.7.1
and Meshtastic 2.8.2: every window with the bytes the part answered, and
the air side it saw, in the format each file's header gives. They are the
reference for the status byte, reset values and reception behaviour above.
The engine differs from them in one place, GetStatus's second byte.

## Not modelled

A crystal module (no TCXO), GFSK and LR-FHSS reception, frequency error,
image calibration and RSSI noise, collisions between injected frames (a new frame replaces one still
on the air), the RX duty cycle's sleep periods (the radio listens
throughout), and BUSY hold times: BUSY drops as soon as the model has
executed a command.
