<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# DUT fixture firmware

The board under test the rig uses to judge the harness: a Zephyr application
for the Raspberry Pi Pico 2 (`rpi_pico2/rp2350a/m33`, the in-tree board with
no Metalclaw changes) or the Pico 1 (`METALCLAW_DUT_BOARD=rpi_pico`), built with `just build-dut [<name>]` into
`build/dut/<name>/zephyr/zephyr.elf`, `<name>` being the device the image was
built for or `smoke` for none.

The image is deliberately inert. It has no console, no shell, no logging and no
USB, and it touches no pin. It boots, announces itself, and answers commands
through a RAM mailbox that the harness's SWD probe reads and writes while the
core keeps running: no halt, no reset, no breakpoint. That is the whole point of
the mailbox. A halting debugger would change the timing the rig is measuring,
and a serial console would need a UART that the DUT's devices are wired to.
The rig reaches the mailbox through its own CMSIS-DAP client
(`test/rig/platforms/hardware/dap.py`): a probe-rs session halts the core
while it attaches and detaches, long enough to lose UART bytes.

```text
CMakeLists.txt      the Zephyr application
prj.conf            what is switched off, which is nearly everything
boards/rpi_pico.conf  what the Pico 1 needs that the Pico 2 board brings
src/mailbox.[ch]    the RAM mailbox
src/main.c          the banner and the commands below
src/uart_echo.c     the UART echo, for an image whose devicetree asks for it
```

The smoke image binds nothing and needs no overlay. A device that needs
DUT-side wiring brings its own from `devices/<name>/test/dut/rp2.overlay`,
which `just build-dut <name>` applies on top of the board: `bmp388` (and the
`bmp388/spi` variant), `ads7883`, `icm4268x`, `neo_m8n`, `sx126x` (and
its `sx126x/native` and `sx126x/raw` variants) and `console_bridge` have one.
A variant comes from `devices/<name>/test/dut/<variant>/rp2.overlay`, built
with `just build-dut <name>/<variant>`; the `cellular` package has one per
model: `cellular/sim7080g`, `cellular/a7672x`, `cellular/sequans_monarch`
and `cellular/sim7000`.

## The UART echo

An image whose devicetree has an `echo-uart` alias, with
`CONFIG_UART_INTERRUPT_DRIVEN` and `CONFIG_RING_BUFFER` on, sends every byte
that UART receives straight back: the interrupt receives into a 4 KiB ring
and a thread below `main`'s priority sends it, from boot for as long as the
image runs. No command starts or stops it, and the
mailbox is unaffected. The `console_bridge` image puts it on the crossed
UART0 pair, so whatever the host writes to the harness's bridged serial port
comes back unchanged at line rate in both directions.

## The mailbox

Two records in RAM, one per direction, each a fixed size the host knows in
advance. The host resolves their addresses by symbol name from the ELF
(`mclw_dut_mailbox_command`, `mclw_dut_mailbox_result`) and moves them with
whole-record `read b32` / `write b32` accesses.

`mclw_dut_mailbox_command`, 128 bytes, host to DUT:

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

`mclw_dut_mailbox_result`, 512 bytes, DUT to host:

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
looking at a target that has not finished `mclw_dut_mailbox_init()` yet, not at a
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
| `RAW <hex> [w] [hz] [n=<count>] [gap=<us>]` | a window on the `raw` alias clocking the given bytes (at most 32); `w` makes it a write-only transfer, `hz` overrides the node's clock; with a count the window repeats `gap` microseconds apart; replies `rx` (the first window), `count` and `mismatches` (later windows whose reply differed) |
| `PULSE <us>` | the `reset` alias's line held active for `us` microseconds, as a host resets a part; the image drives it inactive from boot; replies `us` |
| `LORA CONFIG [key=value]...` | `lora_config()` on the `lora0` alias; keys `freq` (Hz), `bw` (kHz), `sf`, `cr` (5 to 8 for 4/5 to 4/8), `power` (dBm), `preamble`, `tx`, `iq`, `public`, `crc` (0 or 1); a key left out keeps its last value; replies the whole configuration and `errno` |
| `LORA SEND <hex>\|pattern=<len>` | `lora_send()` of the bytes, or of `len` bytes with byte `i` = `i * 7 + 1`; replies `len`, `errno` and `elapsed_ms` |
| `LORA RECV <timeout_ms>` | `lora_recv()`; replies `len`, `rssi`, `snr` and `data` (hex, the first 200 bytes), or `errno` (11 for a timeout, 5 for a CRC error) |
| `LORA AIRTIME <len>` | `lora_airtime()` for the last configuration, in `airtime_ms` |
| `GNSS [RATE\|NAV\|SYSTEMS]` | the `gnss` alias: the last fix the driver published and how many, or one of the driver's pull calls |
| `DNS <host>` | `getaddrinfo` for IPv4: `rc` (0 or a `DNS_EAI_*` code), `ip`, `ms` |
| `TCP\|UDP <host> <port> <hex\|len=N> [count=N] [chunk=N]` | connect, then `count` rounds of sending the payload `chunk` bytes at a time and reading each chunk's echo back before sending the next: `stage` (`done` or where it failed), `errno`, `fd`, `sent`, `received`, `mismatches` (rounds whose echo differed), `crc32` and `head` (first 16 bytes, hex) of the last round's echo, `connect_ms`, `round_ms` (slowest round), `total_ms` |
| `HTTP <host> <port> <path>` | one `GET` with `Connection: close`, read until Content-Length is satisfied, the peer closes, or 20 s: `code`, `status_line`, `header_length`, `content_length`, `body_length`, `body_crc32`, `closed`, `ms` |
| `SOCK OPEN <tcp\|udp> <host> <port>` | a socket held open across commands: `fd` |
| `SOCK SEND <fd> <hex\|len=N>` | `sent` |
| `SOCK RECV <fd> <max> [wait_ms]` | up to `max` bytes within `wait_ms` (default 5000): `received`, `closed`, `crc32`, `head` |
| `SOCK CLOSE <fd>` | `errno` of the close |
| `MODEM INIT` | the `modem` alias: runs the deferred driver init (`device_init`), then brings its network interface up: `rc`, `init_res`, `if_up`, `state`, `ms` |
| `MODEM STATE` | `state`, `initialized`, `ready`, `init_res`, `if_up` |
| `MODEM INFO` | `manufacturer`, `model`, `revision`, `imei`, `iccid` as the driver stored them |
| `MODEM POWER ON\|OFF`, `MODEM NET START\|STOP`, `MODEM GNSS START\|STOP` | the vendor call's `rc` and the `state` after it |
| `MODEM GNSS QUERY` | `rc`, `run`, `fix`, `utc`, `lat`, `lon` (1e-7 deg), `alt` (mm), `hdop` (0.01), `cog` (0.01 deg), `kmh` (0.1 km/h) |
| `MODEM SMS [READ [k]]` | `rc` and `count` of messages read (four at most), their `indices`, and message `k` (default 0): `index`, `stat`, `first_octet`, `pid`, `time` (yy, MM, dd, hh, mm, ss, tz), `length`, `text` |
| `MODEM SMS DELETE <index>` | `rc` |
| `MODEM BATTERY` | `rc`, `bcs`, `bcl`, `mv` |
| `MODEM CELL` | `rc`, `sys_mode`, `op_mode` and the LTE fields of the UE system information |
| `MODEM TIME` | `rc` and the `struct tm` fields exactly as the driver filled them |
| `MODEM FTP <server> <path> <file> [user] [password]` | an FTP get read to the end: `rc` of the start, `result` (`finished`, `error`, `timeout`), `reads`, `bytes`, `crc32` |
| `NET UP [ms]` | powers the device behind the default interface, the `modem` alias (its deferred init the first time, `pm_device_action_run` RESUME after that), brings the interface up and waits up to `ms` (default 60000) for the connection manager's L4 connected event: `pm_op` (`init`, `resume`), `pm_rc`, `up_rc` and the state below; `ok` once L4 is connected |
| `NET DOWN` | the interface down, then a PM SUSPEND of the device, which blocks until the driver has powered the part off: `down_rc`, `pm_rc` and the state below |
| `NET WAIT UP\|DOWN [ms]` | waits up to `ms` (default 30000) for L4 connected or disconnected: `ok` when reached, and the state below |
| `NET STATE` | `admin_up`, `up`, `carrier`, `dormant`, `l4`, `ip` (the interface's IPv4 address), `dns` (the resolver's first IPv4 server), the counts of L4 events `l4_up`, `l4_down`, `dns_add`, of PPP phases `ppp_running`, `ppp_dead`, and the device's `pm` state, `initialized`, `init_res` |
| `CELLULAR INFO` | the `modem` alias through the cellular API: `imei`, `model`, `manufacturer`, `fw`, `imsi`, `iccid` and `rc`, the first error |
| `CELLULAR SIGNAL rssi\|rsrp\|rsrq` | `rc` and `value` (dBm or dB) of `cellular_get_signal`, retried for 2 s while it answers `-EBUSY`, and `tries` |
| `CELLULAR REG [tech]` | `cellular_get_registration_status` for `gsm`, `gprs` (UTRAN) and `lte` (E-UTRAN), or for one numeric `enum cellular_access_technology` as `registration`; an error is in place of the status |
| `CELLULAR APN <apn>` | `rc` of `cellular_set_apn` |
| `CELLULAR NETWORKS` | `rc`, `count` and `networks` (`[technology, bands]` pairs) of `cellular_get_supported_networks` |
| `CELLULAR EVENTS` | what the driver's callback reported since the device came up: `subscribed`, the counts `info`, `registration`, `checks_ok`, `checks_failed`, and `last_registration` |
| `UART <alias> AT <line>` | writes the line and a CR, then reads until a final result: the bytes received ending in `OK` or `ERROR` and CR LF, a whole `+CME ERROR` or `+CMS ERROR` line, or a `>` prompt (with its space); 10 s at most |
| `UART <alias> TX <text>` | writes the text: `written` |
| `UART <alias> WAIT <ms>` | reads until a final result, as `AT` |
| `UART <alias> UNTIL <ms> <text>` | reads until the bytes received end in the text |
| `UART <alias> LINE <ms> <prefix>` | reads until a whole line (to its LF) that starts with the prefix |
| `UART <alias> READ <ms>` | reads what arrives within `ms` |
| `UART <alias> SEND <hex\|len=N>` | writes raw bytes, up to 1024: `sent`, `crc32` |
| `UART <alias> RECV <n> [ms] [crc=<seed>]` | reads exactly `n` raw bytes, up to 4096, within `ms` (default 5000): `received`, `found` (all `n` came), `crc32` seeded with `crc` so a long body can be checked across reads, `head`, and `data` when the bytes fit in the line |
| `UART <alias> FLUSH` | discards what is buffered, `discarded`, and clears `overruns` and `errors` |
| `GPIO <name> SET 0\|1\|PULSE <ms>\|GET` | drives `<name>-gpios` of `/zephyr,user` in logical levels (1 asserted): `level` driven, `read` back, `ms` |
| anything else | `{"test":"test_command","status":"error","detail":"unknown command"}` |

`SENSOR` knows the aliases `barometer` and `imu` and the channels `press`,
`ambient_temp`, `accel_xyz`, `gyro_xyz` and `die_temp`; `SPI` knows the alias
`adc`. Each is one line of a table in `src/main.c`, compiled in only when the
image's devicetree has that alias. `RAW`, `PULSE`, `GNSS` and `LORA` each
serve one alias, `raw`, `reset`, `gnss` and `lora0`.

`LORA RECV` blocks the DUT, mailbox included, until a frame arrives or the
timeout passes; a case that has to put the frame on the air meanwhile sends
it with `dut.start()` and collects the result with `dut.finish()`.

`NET` is the default interface on a native stack and the device the `modem`
alias names, compiled in when the image has the connection manager's L4
events (`CONFIG_NET_CONNECTION_MANAGER`). It is how an image whose driver is
power managed is brought up under test control: the modem node is
`zephyr,deferred-init`, so nothing touches the part until a case has
attached the model and sent `NET UP`, and L4 connected, not an interface
that is up, is what says the address IPCP assigns has arrived. A driver
that powers the part off by itself (modem_cellular after its dial script
gives up) leaves the PM state `active`; `NET DOWN` then `NET UP` restarts it.
`CELLULAR` is `include/zephyr/drivers/cellular.h` on the `modem` alias,
compiled in when the image has `CONFIG_MODEM_CELLULAR`; it subscribes to
the driver's events when `NET UP` initialises the device.

`UART` is raw bytes on a UART no driver owns, for a part Zephyr has no
driver for: the cases speak AT themselves. It knows the alias
`modem-uart`, compiled in when the image has it and an interrupt-driven
UART. Every byte received since boot waits in a 4 KiB ring and each read
takes from it in order, so URCs that arrive between commands are kept; a
read stops at its condition, at its deadline, or when the next byte would
not fit in the result line, and what it did not take stays for the next
read. Text reads answer `data` (the bytes as a JSON string, `\r`, `\n`,
`\t` and `\u00XX` escaped), `length`, `found` (the condition was met),
`more` (the line filled first), and every `UART` answer carries `pending`
(bytes still buffered), `overruns` (bytes lost to a full ring), `errors`
(the UART's error flags, ORed) and `ms`. Text arguments take `\r`, `\n`,
`\t`, `\\` and `\xHH`, and the rest of the command line is the text,
spaces included. `GPIO` knows the pin `pwrkey`, compiled in when
`/zephyr,user` has `pwrkey-gpios`.

`DNS`, `TCP`, `UDP`, `HTTP` and `SOCK` are the socket API, compiled in when
the image has `CONFIG_NET_SOCKETS`, on whatever stack it carries, native or
offloaded; they answer `network interface down` until the default interface
is up. Every wait in them is a `poll()` with a deadline, because an
offloading driver may ignore `SO_RCVTIMEO`. `len=N` sends a printable
pattern, byte `i` being `0x21 + (7 i mod 94)`. `MODEM` is the vendor API of
Zephyr's SIM7080 offload driver (`drivers/modem/simcom/sim7080`), compiled in
when the image has that driver and a `modem` alias. CRC-32s are the IEEE one
(`zlib.crc32`).

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
