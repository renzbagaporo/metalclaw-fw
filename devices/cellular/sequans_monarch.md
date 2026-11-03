<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# sequans_monarch

A Sequans Monarch LTE-M/NB-IoT modem on UART, with its RESETN. The harness
answers the commands of the Monarch Platform LR5.1.1.0 AT Commands
Reference Manual Rev 6 (GM01Q, "LR5") that TinyGSM's Sequans Monarch client
sends, and those of the Monarch 2 Platform LR8.2 AT Commands Reference
Manual Rev 3 (GM02S, "LR8.2") that Zephyr's `modem_cellular` driver
(`sqn,gm02s`) sends, so the board under test can boot the modem, register,
define and activate PDP contexts, open TCP and UDP sockets in command mode,
multiplex the UART with CMUX and dial PPP, read the clock and the
thermistor and send SMS, against the package's
[fake network](README.md#the-fake-network).

`sequans_monarch.c` holds the descriptors, the config and the model's own
commands; `sequans_monarch_sockets.c` is the `SQNS*` socket chapter. The
engine, the standard commands, CMUX and PPP, SMS and the network are the
package's ([README](README.md)). The model has the modem, network, SMS and
link parts and its own: no GNSS.

## Attaching

The UART binding advertises 115200 baud 8N1. The second binding is
`reset`, the module's RESETN, active low. On fixture 1 the crossed pair is
GP0/GP1 and the DUT's GP6 drives the harness's GP6:

```python
import metalclaw
modem = metalclaw.attach("sequans_monarch", "modem", uart=(0, 1), reset=6)
modem.write_region("hosts", 0, b"vsh.pp.ua=192.0.2.80\nbroker.hivemq.com=192.0.2.18\n\0")
modem.write_region("file", 0, open("test_1k.bin", "rb").read())
modem.set(file_length=1024, temperature=31)
modem.powered, modem.pdp, modem.connections, modem.bytes_down
modem.reset()
modem.detach()
```

`pdp` is the bearer: up while a context is active (`CGACT`) or PPP's IPCP
is open. `connections` counts successful `SQNSD`s and the link's TCP
connections, `registration` is the `+CEREG` stat, `boot_ms` defaults to
1500 and `power_cycles` counts `+SYSSTART`s.

The model's own part:

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `temperature` | i32 | rw | °C | -40 .. 85 | `+SMDTH`, default 25 |

The state takes 8376 bytes on the RP2040, 131 arena pages of 64.

## Power and timing

| from | event | to |
| --- | --- | --- |
| attach or `reset()` | | booting, with or without `reset` |
| booting or on | RESETN low for 100 µs | off, losing what had not been sent |
| off with RESETN low | RESETN released | booting |
| booting | `boot_ms` later | `+SYSSTART`; on, at CFUN 0 with echo off |
| on | `AT+CFUN=<fun>,1` | `OK`, `+SHUTDOWN`, booting; on again at `<fun>` |
| on | `AT^RESET` | `+SHUTDOWN` without an `OK` (the manuals list none), booting; on again at CFUN 0 |
| on | `AT+SQNSSHDN` | `OK`, `+SQNSSHDN`, off until RESETN or `powered` |

Zephyr's resume pulses RESETN for 100 ms and sends `ATE0` 2 s after the
release, without probing; `+SYSSTART` comes 1.5 s after the release. The
baud rate survives a boot. Every answer and URC is sent from `on_rx` or the
next `time_advanced`: nothing trails its command on purpose. One `+SQNSRING`
and one `+SQNSH` wait per socket, and a socket's pending URCs go when it is
closed or dialled again. Input waits while the output FIFO has less than
400 bytes free, room for an `AT+CGDCONT?` list of six contexts; a URC waits
for 32.

What the part keeps in NVRAM is kept across boots and cleared by `reset()`:
the contexts of `CGDCONT`, the socket settings of `SQNSCFG`, `SQNSCFGEXT`
and `SQNSSCFG`, `CTZR`, `CTZU` and `SQNIPSCFG`. Everything else is lost at
a boot.

## Commands

Test forms (`=?`) answer `OK` or the manual's ranges.

| command | answer |
| --- | --- |
| `AT`, `ATE0`/`ATE1`, `ATZ`, `AT&F`, `AT&W` | `OK`; echo is off at boot |
| `ATV0`, `ATV1` | `OK`; on the channel it came from, `ATV0` turns to numeric result codes and `ATV1` back |
| `AT^RESET` | see above |
| `ATI` | `SEQUANS Communications`, `GM02S`, `UE8.2.0.5-60964` on three lines |
| `AT+CGMI` | `SEQUANS Communications` |
| `AT+CGMM` | `GM02S` |
| `AT+CGMR` | `UE8.2.0.5-60964` |
| `AT+CGSN`, `AT+CGSN=0`, `AT+GSN` | `354658090123453` |
| `AT+CGSN=1` | `+CGSN: "354658090123453"` |
| `AT+CIMI` | `001010123456789`; CME 10 at CFUN 0 |
| `AT+SQNCCID`, `AT+SQNCCID?` | `+SQNCCID: "89882280666027595036",""`; `OK` alone at CFUN 0; CME 10 without a SIM |
| `AT+CMEE=<n>`, `?` | 0, 1, 2 |
| `AT+IPR=<rate>`, `?` | 9600 to 921600; `OK`, then the switch; 0 is `ERROR` |
| `AT+CFUN=<fun>[,<rst>]`, `?` | 0, 1, 4 and 5; 1 registers, the others drop the network; `rst` 1 reboots (see above) |
| `AT+CPIN?`, `=<pin>` | `+CPIN: READY` or `SIM PIN`, any PIN unlocks; CME 10 at CFUN 0 or without a SIM |
| `AT+CSQ` | `+CSQ: <rssi>,99`; `99,99` below CFUN 1 |
| `AT+CESQ` | `+CESQ: 99,99,255,255,20,<rsrp>` at CFUN 1, RSRP 10 dB below the rssi's -113 + 2n dBm coded as 27.007 tables it (index n for [-141 + n, -140 + n) dBm); `255,255` otherwise |
| `AT+CEREG=<n>`, `?` | `+CEREG: <n>,<stat>`; `n` 0 to 5, every non-zero `n` sends `+CEREG: <stat>` on a change |
| `AT+CREG=<n>`, `?` | the same as `+CREG`, `n` 0 to 2 |
| `AT+COPS?` | `+COPS: 0,0,"Metalclaw",7`; `+COPS: 0` unregistered |
| `AT+CTZR=<0..2>`, `AT+CTZU=<0/1>`, `?` | stored; no `+CTZV` or `+CTZE` is sent |
| `AT+CCLK?`, `=` | `+CCLK: "yy/MM/dd,hh:mm:ss±zz"`, local time (`time` plus `timezone`); the write sets both |
| `AT+SMDTH` | `+SMDTH: <temperature>`; a write with a value is `ERROR` |
| `AT+SQNIPSCFG=<mode>[,<timeout>]`, `?` | stored, default `1,5000`; the UART never sleeps |
| `AT+CGDCONT=<cid>,"IP"/"IPV6"/"IPV4V6"[,"<apn>"]`, `=<cid>`, `?` | cid 1 to 6, APN up to 31 characters; the second form undefines; CME 3 while attached; cid 1 is `"IPV4V6",""` from the factory |
| `AT+CGAUTH=<cid>[,<0..2>[,...]]` | `OK`; the credentials are not kept |
| `AT+CGACT=1[,<cid>...]` | registered: attaches, activates the contexts (all defined ones without a cid), the bearer comes up; CME 30 unregistered |
| `AT+CGACT=0[,<cid>...]`, `?` | deactivates; CME 3 for the last active context while attached (an EPS PDN), the PPP call's counting; the last one takes the bearer and every socket, and the call's context ends the call; the read form shows a context active while `CGACT` or the PPP call holds it |
| `AT+CGATT=<0/1>`, `?` | 0 detaches, ending every context and socket |
| `AT+CGPADDR[=<cid>...]` | `+CGPADDR: <cid>,"<ip>"` per active context, `+CGPADDR: <cid>` per inactive one |
| `AT+CMUX=0[,...]` | `OK`, then the engine's multiplexer |
| `ATD*99***<cid>#` | attached: `CONNECT <baud>` and PPP on that channel; `NO CARRIER` otherwise. The call holds `<cid>` active until it ends; its end leaves the bearer to the contexts `CGACT` activated, and with none the sockets go |
| `AT+SQNSSHDN` | see above |
| `AT+CUSD=...` | `ERROR` |
| `AT+CMGF`, `AT+CSCS`, `AT+CSMP`, `AT+CNMI`, `AT+CMGS`, `AT+CMGL`, `AT+CMGD` | the package's SMS |
| `AT+CCID`, `AT+CGREG`, `AT+CBC` | `ERROR`, unknown: neither manual has them |

## Sockets

Sockets 1 to 6 are the network's sockets 0 to 5. A socket the PPP link's
TCP holds reads as closed and cannot be dialled.

| command | answer |
| --- | --- |
| `AT+SQNSCFG=<id>,<cid>,<pktSz>,<maxTo>,<connTo>,<txTo>`, `?` | stored; `<cid>` (default 1) is the context `SQNSD` needs active; the timeouts are not modelled |
| `AT+SQNSCFGEXT=<id>,<srMode>,<recvDataMode>,<keepalive>[,<listenAutoRsp>[,<sendDataMode>...]]`, `?` | stored; `srMode` 2 is CME 4; `recvDataMode` shapes only `srMode` 2 |
| `AT+SQNSPCFG=<spId>[,...]`, `AT+SQNSPCFG` | `OK`; the profile is not kept |
| `AT+SQNSSCFG=<id>,<enable>[,<spId>]`, `?` | stored; CME 3 while the socket is connected; TLS reaches the same plaintext services |
| `AT+SQNSD=<id>,<0 TCP/1 UDP>,<port>,"<host>"[,<closure>[,<lport>[,1[,<any>[,0]]]]]` | `OK` once connected; `NO CARRIER` when the socket's context is not up, the name does not resolve or nothing listens; CME 3 while connected; CME 4 for online mode (`connMode` 0, the default) or `connSetup` 1 |
| `AT+SQNSSENDEXT=<id>,<n>` | `\r\n> `, then `n` (1 to 1500) bytes, raw or as `2n` hex digits as `sendDataMode` says, then `OK`; NUL, CR and LF between hex digits are skipped, any other byte that is not a hex digit ends it with `ERROR`; `ERROR` 10 s after the prompt if the data has not all come; CME 3 unless connected |
| `AT+SQNSRECV=<id>,<max>` | `+SQNSRECV: <id>,<n>`, CR LF, `n` raw bytes, CR LF, `OK`; `n` at most what the output FIFO can take (about 670); `ERROR` with nothing buffered |
| `AT+SQNSI=<id>` | `+SQNSI: <id>,<sent>,<received>,<buffered>,0` since the dial; `received` counts what arrived, read or not |
| `AT+SQNSI` | the same for all six |
| `AT+SQNSS`, `AT+SQNSS?` | six lines: `+SQNSS: <id>,2,<ip>,<40000+id>,<remote ip>,<port>` connected, `3` with data buffered, `+SQNSS: <id>,0` closed |
| `AT+SQNSH=<id>` | `OK`; the socket and its buffered data are gone; `OK` for a closed one |

`+SQNSRING: <id>,<n>` (`srMode` 1) or `+SQNSRING: <id>` (`srMode` 0) is sent
when a socket's data goes from none to some, `n` what is buffered when it
leaves, at most 1500. A server close shows once the DUT
has read every byte: the socket reads `3` until then, and then `0` with a
`+SQNSH: <id>`. Losing the network, CFUN 0 or 4, `CGATT=0` or deactivating
the last context ends every connected socket at once, its data discarded,
each with a `+SQNSH: <id>`.

## The Zephyr driver this is built against

Zephyr v4.4.0, `drivers/modem/modem_cellular.c` with `sqn,gm02s`. Without
`autostarts` (a devicetree boolean the instance macro reads as false), its
resume and every init failure pulse `mdm-reset-gpios` for 100 ms and wait
2 s. The init script on the raw UART: `ATE0`, `AT+CFUN=4`, `AT+CMEE=1`,
`AT+CEREG=1`, `AT+CEREG?`, then `AT+CGSN`, `AT+CGMM`, `AT+CGMI` and
`AT+CGMR`, read by catch-all matches that take any line, so each answers
one bare line with echo off; then `AT+CMUX=0,0,5,127`, which must answer
`OK`. Then the APN script on DLCI 1 (`AT+CGDCONT=1,"IP","<apn>"`), the
dial script on DLCI 1 (`AT+CGACT=0,1` taking `OK` or `ERROR`, `AT+CFUN=1`
and a fixed 10 s wait, `ATD*99***1#` waiting for `CONNECT`), and the
periodic script on DLCI 2 (`AT+CEREG?` every 2 s). There is no shutdown
script. `cellular_get_signal()` sends `AT+CSQ` and `AT+CESQ`.

The quirks the model accommodates:

- `+SYSSTART` comes 1.5 s after RESETN is released, before the script's
  first command, which it does not match.
- `AT+CGDCONT` is allowed at CFUN 4, before the dial attaches.
- No `+CGEV` lines: the driver reads them as registration changes.

## TinyGSM

TinyGSM 0.12's `TinyGsmClientSequansMonarch` (`TINY_GSM_MODEM_SEQUANS_MONARCH`)
works against the model:

- `restart()` gets `OK` for `AT+CFUN=0` and `+SYSSTART` after
  `AT+CFUN=1,1`; `init()` turns CFUN 1 on, and `AT+CPIN?` answers once it
  has.
- `gprsConnect()` detaches, deactivates every context (nothing is active
  then, so no last-PDN refusal), defines context 3 `IPV4V6`, activates it,
  which attaches, and attaches; `localIP()` reads the quoted `+CGPADDR: 3,"<ip>"`.
- `getSimCCID()` returns `"89882280666027595036",""` with the quotes.
- Every `write()` is one `SQNSSENDEXT` in hex with a NUL after each digit
  pair; the last NUL comes after the `OK` and is dropped as noise.
- `+SQNSRING: <id>,<n>` sets the available count, which every read
  refreshes with `SQNSI`; `SQNSS` answers six lines so it never waits out
  a missing one.
- `getTemperature()` waits for `+SMDTH: ` with a space.
- `sendUSSD()` gets `ERROR` at once; `poweroff()` gets `OK`.

## Where the sources disagree

- Echo: TinyGSM's notes have the part boot with echo on; ATE defaults to 0
  in both LR5 and LR8.2, so echo is off at boot. Neither host depends on it.
- `AT+SQNSSHDN`: LR5 ends with a `+SQNSSHDN` URC, LR8.2 with `+SHUTDOWN`;
  the model sends LR5's.
- `+SQNSS`: LR8.2 adds a seventh field, `<txProt>`; the model sends LR5's
  six.
- `+SQNSI`: LR8.2 adds an optional `<ack_notif>`; the model sends LR5's
  five fields.
- `+SMDTH`: LR5 prints no space after the colon; TinyGSM needs it, and the
  model sends it.
- `AT+SQNCCID`: LR5 lists only the read form, LR8.2 both; both answer, the
  ICCID quoted as TinyGSM expects.
- `AT+CMUX`: LR8.2 says the multiplexer runs at 921600 baud; Zephyr does not
  change its rate, and the model does not either.

## Not modelled

Online mode (`SQNSD` `connMode` 0, `CONNECT`, `+++`, `SQNSO`), `SQNSSEND`,
`closureType` 255, `srMode` 2, listening sockets (`SQNSL`, `SQNSLUDP`,
`SQNSA`), LR8.2's asynchronous dial and `+SQNSD` URC, the socket timeouts of
`SQNSCFG`, the TLS handshake and the profiles' contents, `+CGEV`, `+CTZV`
and `+CTZE`, the location fields of `+CEREG`, IPv6 addresses, PDP context 0
and contexts above 6, the HTTP, MQTT, CoAP, ping, NTP, file and
firmware-upgrade application commands, GNSS, calls, incoming SMS and
`+CMTI`, `ATI1` to `ATI3`, `AT+CGSN=2` and `3`, PSM, eDRX, UART sleep and
flow control, autobauding, command concatenation with `;`, and a real
module's time to register. The clock in `time` does not run.

## Tests

The host dialogues replay TinyGSM's Sequans transcript (restart, `init`,
`gprsConnect`, a 1 KiB HTTP download over `SQNSRING`/`SQNSRECV` with CRC
0x6f50d767, the same over TLS on 443, MQTT, SMS, `SMDTH`, power down),
`modem_cellular`'s `sqn,gm02s` init script up to `AT+CMUX`, a PPP call over
CMUX that holds context 1 beside `CGACT`, and `AT^RESET`, under ASan and
UBSan. `test/cases/test_sequans_monarch.py` runs on the
`cellular/sequans_monarch` DUT image, `test/dut/sequans_monarch/`: Zephyr's
`modem_cellular` with `sqn,gm02s` over CMUX and PPP, RESETN on GP6.

The image's `rp2.overlay` puts `uart0` on GP0/GP1 at 115200, removes the
console from it, and hangs a `sqn,gm02s` node aliased `modem` under it with
`mdm-reset-gpios` on GP6, active low, its RESETN, and no `autostarts`: the
property is a devicetree boolean, so the driver's default of true never
applies, and every resume pulses RESETN for 100 ms and waits 2 s. The node
is `zephyr,deferred-init`: modem_cellular's init resumes the device at
once, which is a pulse on GP6 and a startup wait, and the rig flashes and
pings the DUT before a case attaches the model; `NET UP` runs the init. A
GPIO hog holds GP6 released from boot until the driver configures it: an
RP2350 pad resets with its pull-down on, and the harness's input has no
pull of its own, so until the hog the line is a held reset. A DUT reset
therefore resets the part too. `rp2.conf` turns on
`CONFIG_MODEM_CELLULAR` with the APN `internet`, device PM, Zephyr's native
IPv4 stack over PPP (TCP, UDP, sockets, the DNS resolver fed by IPCP, no
IPv6, no Ethernet), the connection manager for L4 events, and
`CONFIG_MODEM_CMUX_WORK_BUFFER_SIZE_EXTRA=512`: the DLCI receive rings are
the CMUX work buffer size, and at the default two full UIH frames in one
UART batch overrun them. The image takes 122312 bytes of flash and 74376 of
RAM.

`NET UP` initialises the device (a PM resume after the first time), brings
the PPP interface up and waits for the connection manager's L4 connected,
which needs the address IPCP assigns; `NET DOWN` takes the interface down
and suspends the device, which closes CMUX and powers the part off as the
driver does it. `CELLULAR` reads the driver through the cellular API, and
`DNS`, `TCP`, `UDP`, `HTTP` and `SOCK` run on Zephyr's IP stack. The
reference is `test/rig/dut/README.md`.

```sh
just build-dut cellular/sequans_monarch
METALCLAW_HARNESS_SERIAL=<serial> METALCLAW_ARM_TOOLCHAIN_DIR=<dir> \
METALCLAW_DUT_DEVICE=cellular/sequans_monarch just test hardware
```

`test/cases/test_sequans_monarch.py` subclasses the package's
`PppModelCases` (`common.py`). It resets the DUT, attaches the model at
`uart=(0, 1)` and `reset=6`, writes three names into `hosts` and a
printable 1 KiB `file`, and runs `NET UP`, then checks: the part booted once from
attach, and the pulse brought it up once with CMUX and PPP up, PDP active
and no unknown command; the address and DNS server IPCP handed over; the
modem info the init script read (IMEI, model, manufacturer and firmware);
the registration the driver caches per technology (`+CEREG` 1; `CREG` and
`CGREG` are never asked and stay 0) and `-ENODATA` for 5G; `rssi` through
`AT+CSQ` at 0, 17 and 31, unknown as `-EINVAL`, and RSRP and RSRQ through
`AT+CESQ` (RSRP one dB above the model's, as the driver reads the 27.007
index, RSRQ -10 dB); the driver's events, every periodic check over two
periods answered on DLCI 2; the APN fixed once dialled; no configurable
networks; DNS for every `hosts` name, a literal, and an unknown name failing
at once with `EAI_FAIL`, Zephyr's code for an answer without records; TCP echo of 1
to 1000 bytes, three rounds each, intact by CRC with the model's counters;
the PPP and CMUX flag bytes through the echo; UDP echo up to 256 bytes; an
HTTP GET of the file intact with the request line in `request`, a 404, and
a refused port (`ECONNREFUSED`); no frame with a bad FCS; a short loss of
registration (the driver goes dormant, PPP closes, and it redials without a
power cycle); a long one (three failed dials, the driver closes CMUX and
powers down by itself; with no shutdown script and no power line it leaves
the part powered, then `NET DOWN` and `NET UP` restart it); and power off
and on through `NET DOWN` and `NET UP`, with no unknown command through all
of it. A case of its own resets the DUT with the part in CMUX and PPP,
finds the part rebooted with it, and the fresh driver's RESETN pulse
rebooting it once more and bringing the link back.
