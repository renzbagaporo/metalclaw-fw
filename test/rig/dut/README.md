<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# DUT fixture firmware

The board under test the rig uses to judge the harness: a Zephyr application
for the Raspberry Pi Pico 2 (`rpi_pico2/rp2350a/m33`, the in-tree board with
no Metalclaw changes), built with `just build-dut [<name>]` into
`build/dut/<name>/zephyr/zephyr.elf`, `<name>` being the device the image was
built for or `smoke` for none.

The image is deliberately inert. It has no console, no shell, no logging and no
USB, and it touches no pin. It boots, announces itself, and answers commands
through a RAM mailbox that the harness's SWD probe reads and writes while the
core keeps running: no halt, no reset, no breakpoint. That is the whole point of
the mailbox. A halting debugger would change the timing the rig is measuring,
and a serial console would need a UART that the DUT's devices are wired to.

```text
CMakeLists.txt      the Zephyr application
prj.conf            what is switched off, which is nearly everything
src/mailbox.[ch]    the RAM mailbox
src/main.c          the banner and the commands below
```

The smoke image binds nothing and needs no overlay. A device that needs
DUT-side wiring brings its own from `devices/<name>/test/dut/rp2.overlay`,
which `just build-dut <name>` applies on top of the board: `bmp388` (and the
`bmp388/spi` variant), `ads7883`, `icm4268x` and `neo_m8n` have one.

## The mailbox

Two records in RAM, one per direction, each a fixed size the host knows in
advance. The host resolves their addresses by symbol name from the ELF
(`mclw_debug_port_command`, `mclw_debug_port_result`) and moves them with
whole-record `read b32` / `write b32` accesses.

`mclw_debug_port_command`, 128 bytes, host to DUT:

| offset | size | field | value |
| --- | --- | --- | --- |
| 0x00 | 4 | `magic0` | `0x574C434D` (`MCLW`) |
| 0x04 | 4 | `magic1` | `0x31444D43` (`CMD1`) |
| 0x08 | 4 | `version` | 1 |
| 0x0c | 4 | `capacity` | 100 |
| 0x10 | 4 | `sequence_begin` | the command's sequence number |
| 0x14 | 4 | `length` | payload bytes, 1 to 100 |
| 0x18 | 100 | `payload` | ASCII, no terminator, no newline |
| 0x7c | 4 | `sequence_end` | equal to `sequence_begin` |

`mclw_debug_port_result`, 512 bytes, DUT to host:

| offset | size | field | value |
| --- | --- | --- | --- |
| 0x00 | 4 | `magic0` | `0x574C434D` (`MCLW`) |
| 0x04 | 4 | `magic1` | `0x31534552` (`RES1`) |
| 0x08 | 4 | `version` | 1 |
| 0x0c | 4 | `capacity` | 484 |
| 0x10 | 4 | `sequence_begin` | the result's sequence number |
| 0x14 | 4 | `length` | payload bytes, 0 to 484 |
| 0x18 | 484 | `payload` | one ASCII line, zero-padded |
| 0x1fc | 4 | `sequence_end` | equal to `sequence_begin` |

Both records are zeroed at boot and their headers written afterwards, `magic0`
last. A reader that sees a header which is not exactly the four words above is
looking at a target that has not finished `mclw_debug_port_init()` yet, not at a
protocol error.

### Sequence rules

Neither side locks the other out, so both records carry the same sequence number
twice and every reader checks that the two copies agree.

- The host writes a command as one record with `sequence_begin` equal to
  `sequence_end`, numbering from 1 and strictly increasing for the life of the
  image. The DUT consumes a record only when the header matches, `length` is
  1 to 100 and fits the reader's buffer, the two sequence words agree, and the
  number is not the one it consumed last. A command is dropped, not queued, if
  the next one arrives before the DUT polls.
- The DUT publishes a result by writing `sequence_begin` first, then the payload
  and `length`, then `sequence_end`, with a memory barrier before and after the
  payload. A host that reads the record and finds the two sequence words
  disagreeing has caught a publication in flight; it retries. `sequence_begin`
  of 0 means the image has published nothing yet.
- The DUT does not echo which command a result answers. The host tells replies
  apart by sequence: a result whose number is greater than the one read before
  sending is new.

### Lines

One result is one line. The DUT buffers written text until it sees `\n`, then
publishes the buffered line as a single record, so a partially formatted result
is never visible. Every line this image emits has the form

```text
MCLW-HIL/1 {"test":"...","status":"ok|error",...}\r\n
```

A JSON object with string values only. `test` names what the line reports,
`status` is `ok` or `error`, and an error line carries `detail`.

## Commands

A command is one line of ASCII with no newline, at most 100 bytes.

The commands are per Zephyr subsystem, not per part: each names a devicetree
alias the image was built with and says what to do with it. What a value
means is the case's business, not the image's.

| command | reply |
| --- | --- |
| (boot, unprompted) | `{"test":"test_boot","status":"ok","image":"smoke","revision":"1","build":"<date time>","boots":<n>}` |
| `PING` | `{"test":"test_ping","status":"ok","image":"smoke"}` |
| `ID` | the boot line's fields, with `"test":"test_id"` |
| `SENSOR <alias> <channel>... [count=N]` | one fetch through the sensor API, each named channel as a JSON array of micro-units from `sensor_value_to_micro`; with a count, `N` fetches, the first reported and `mismatches` counting the later ones that differed |
| `SPI <alias> [hz] [bytes] [count] [gap_us]` | `count` chip-select windows of `bytes` bytes through the SPI API at `hz` (0 for the node's own clock), `gap_us` apart: the first `frame` as hex and `mismatches` counting the later windows that differed |
| `RAW <hex> [w]` | one window on the `raw` alias clocking the given bytes; `w` makes it a write-only transfer; replies `rx` |
| `GNSS [RATE\|NAV\|SYSTEMS]` | the `gnss` alias: the last fix the driver published and how many, or one of the driver's pull calls |
| anything else | `{"test":"test_command","status":"error","detail":"unknown command"}` |

`SENSOR` knows the aliases `barometer` and `imu` and the channels `press`,
`ambient_temp`, `accel_xyz`, `gyro_xyz` and `die_temp`; `SPI` knows the alias
`adc`. Each is one line of a table in `src/main.c`, compiled in only when the
image's devicetree has that alias.

A command naming an alias the image was not built with answers
`"status":"error"` with a `detail`; one whose device did not initialise
answers `errno` 19 and `init_res`, the driver's init return code.

`build` is the image's compile timestamp. It is what makes a stale image
obvious: a reflash that did not take keeps the old timestamp, and a board that
re-enumerates proves nothing. `image` is the device (or `device/variant`) the
image was built for, `smoke` for none; `just build-dut` passes it in.

The loop polls for a command, answers it, and sleeps 1 ms. `main()` never
returns: a silent board must mean a dead board, never a board that fell out of
its loop.
