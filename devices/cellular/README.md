<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# cellular

Cellular modems on a UART with their PWRKEY: the board under test talks AT
to the harness, which answers as the part would and puts a small fake
network behind it. One package holds the engine every model shares and the
models themselves; each model is attached by its part name.

| model | part | page |
| --- | --- | --- |
| `sim7080g` | SIMCom SIM7080G LTE-M/NB-IoT | [sim7080g.md](sim7080g.md) |
| `sim7000` | SIMCom SIM7000 series LTE-M/NB-IoT | [sim7000.md](sim7000.md) |
| `a7672x` | SIMCom A7672X (A76XX) LTE Cat-1 | [a7672x.md](a7672x.md) |
| `sequans_monarch` | Sequans Monarch (GM01Q) and Monarch 2 (GM02S) LTE-M/NB-IoT | [sequans_monarch.md](sequans_monarch.md) |

```text
modem.h              the engine's API: state, config, channels, parts, descriptor building blocks
modem.c              power (PWRKEY or RESET_N), the output FIFO, the URC queue, the model ops, the modem part
at.c                 AT lines per channel, data phases, dispatch, result framing, +CME/+CMS
link.c               CMUX channels, the PPP transmit scheduler, +CMUX and ATD, the link part
cmux.[ch]            27.010 basic option: frame parser, encoder, the DLCI 0 responder
ppp.[ch]             HDLC framing, FCS-16, the LCP and IPCP responders
ip.[ch]              IPv4, ICMP echo, checksums, the stack's packet scheduler
tcp.[ch]             the TCP responder over the network's services
udp.[ch]             UDP: DNS, echo, discard
dns.[ch]             the DNS server over the hosts region
standard.c           the 27.007/V.250 commands every model answers; SIM and registration
text.[ch]            integer formatting and matching without printf
net.[ch]             the fake network: hosts, sockets, echo, discard, HTTP, MQTT, FTP's request record
parts.c              the property and region parts of the network and the GNSS fix
gnss.h               the GNSS fix
sms.[ch]             27.005 SMS: the store, CMGF, CMGS, CMGL, CMGD, and its part
sim7080g.[ch]        the SIM7080G: descriptors, config, its own commands and URCs
sim7080g_sockets.c   the SIM7080G's CA* socket dialect and CDNSGIP
sim7000.[ch]         the SIM7000: descriptors, config, the bearer (SAPBR, CGACT, CNACT), GNSS, CLBS, NTP, USSD
sim7000_ca.c         the SIM7000's CA* socket stack, as TinyGSM's SIM7000SSL driver speaks it
sim7000_cip.c        the SIM7000's CIP* stack in multi-IP mode, and what both stacks share
a7672x.[ch]          the A7672X: descriptors, config, context 1, CLBS, NTP, USSD, its URCs
a7672x_sockets.c     the A7672X's TCP/IP chapter: NETOPEN, the CIP* links, CDNSGIP
a7672x_ssl.c         the A7672X's SSL chapter: CSSLCFG and the CCH* sessions
sequans_monarch.[ch] the Monarch: descriptors, config, CFUN and reboots, PDP contexts, CESQ, SMDTH
sequans_monarch_sockets.c  the Monarch's SQNS* sockets
test/cases/          common.py (shared helpers and PppModelCases) and test_<model>.py per model
test/dut/<model>/    rp2.overlay and rp2.conf of the model's DUT image
```

Everything runs on the worker under the runtime lock (`on_rx`,
`time_advanced`, `poll`) or on the script's thread (`get`, `set`,
regions); no model code runs in an interrupt.

## Bindings

Every model has two: the UART, and its power pin, which is the module's
PWRKEY or, on a part driven by its reset line, RESET_N.

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `uart` | `uart` | yes | a UART, from the tuple `(tx, rx)`, at the part's fixed rate, 8N1 |
| 1 | `pwrkey` or `reset` | `gpio_in` | no | the module's PWRKEY or RESET_N, active low, from one pin |

`sequans_monarch` has `reset`; the others have `pwrkey`.

```python
import metalclaw
modem = metalclaw.attach("sim7080g", "modem", uart=(0, 1), pwrkey=6)
gm02s = metalclaw.attach("sequans_monarch", "modem", uart=(0, 1), reset=6)
```

Without `pwrkey` the modem boots as soon as it is attached. A model with
`reset` boots at attach whether the pin is wired or not.

## Properties

A model's properties are the parts it has, in this order. Every model has
the modem part and the network part.

The modem part:

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `powered` | bool | rw | | | the modem is booting or on; true boots an off modem, false cuts power without the power-down message, losing output not yet handed to the UART engine |
| `boot_ms` | u32 | rw | ms | 0 .. 20000 | power-on to ready |
| `baud` | u32 | rw | baud | 9600 .. 921600 | the rate the UART runs at; `AT+IPR` moves it too |
| `echo` | bool | ro | | | command echo (`ATE`) |
| `functionality` | u32 | ro | | 0 .. 7 | `AT+CFUN` level |
| `sim` | u32 | rw | | 0 .. 2 | 0 ready, 1 not inserted, 2 PIN required; a change locks the SIM again and, while on, sends `+CPIN: READY`, `NOT INSERTED` or `SIM PIN` |
| `registration` | u32 | rw | | 0 .. 5 | the network's registration stat, default 1 |
| `rssi` | u32 | rw | | 0 .. 31, 99 | `+CSQ` rssi, default 20 |
| `time` | i32[6] | rw | | | UTC year (1980 .. 2099), month, day, hour, minute, second, default 2026-01-01 00:00:00; an impossible date is refused; it does not run |
| `timezone` | i32 | rw | 15 min | -48 .. 56 | local time offset for `+CCLK` |
| `battery_mv` | u32 | rw | mV | 0 .. 5000 | default 3800 |
| `battery_percent` | u32 | rw | % | 0 .. 100 | default 75 |
| `commands` | u32 | ro | | | command lines received |
| `unknown_commands` | u32 | ro | | | lines answered `ERROR` because the command is not modelled |
| `power_cycles` | u32 | ro | | | boots completed |

The network part:

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `pdp` | bool | ro | | | the data bearer is up |
| `ip` | u32[4] | rw | | 0 .. 255 | the address the bearer gets, default 10.0.0.2 |
| `file_length` | u32 | rw | bytes | 0 .. 1024 | how much of `file` the HTTP and FTP servers serve |
| `connections` | u32 | ro | | | sockets opened |
| `bytes_up` | u32 | ro | bytes | | socket payload the DUT sent |
| `bytes_down` | u32 | ro | bytes | | socket and FTP payload the DUT read |
| `drops` | u32 | ro | bytes | | bytes lost to a full socket receive buffer |
| `mqtt_publishes` | u32 | ro | | | `PUBLISH` packets the broker received |
| `mqtt_send` | bool | wo | | | true publishes `mqtt_inject` to every subscriber |

The GNSS part, on a model with a receiver:

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `latitude` | i32 | rw | 1e-6 deg | ±90e6 | |
| `longitude` | i32 | rw | 1e-6 deg | ±180e6 | |
| `altitude` | i32 | rw | mm | ±99999999 | MSL altitude |
| `speed` | u32 | rw | 0.01 km/h | 0 .. 99999 | speed over ground |
| `course` | u32 | rw | 0.1 deg | 0 .. 3599 | course over ground |
| `hdop` | u32 | rw | 0.1 | 0 .. 999 | default 1.0 |
| `satellites` | u32 | rw | | 0 .. 99 | satellites in view |
| `fix` | bool | rw | | | the receiver has a fix |
| `gnss_power` | bool | ro | | | the receiver is on |

The SMS part, on a model with SMS: `sms_sent` (u32, ro), messages sent.

The link part, on a model with CMUX and PPP:

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `mru` | u32 | rw | bytes | 128 .. 576 | the MRU the model asks for in LCP, default 296; the DUT's TCP MSS follows it (MRU - 40); taken at the next LCP negotiation |
| `gw` | u32[4] | rw | | 0 .. 255 | the model's own address in IPCP, default 10.0.0.1 |
| `dns` | u32[4] | rw | | 0 .. 255 | the DNS server IPCP hands the DUT, default 10.0.0.1 |
| `cmux` | bool | ro | | | the multiplexer is up |
| `ppp_up` | bool | ro | | | LCP and IPCP are open |
| `lcp_terminates` | u32 | ro | | | LCP Terminate-Requests, from either side |
| `tcp_retransmits` | u32 | ro | | | segments the model sent again after its RTO |
| `frames_bad_fcs` | u32 | ro | | | CMUX and PPP frames dropped for their FCS |
| `dlci_fc_stalls` | u32 | ro | | | times the DUT's MSC flow control stopped the PPP DLCI |

The network part's `ip` is the address IPCP gives the DUT, and `pdp` is
true while IPCP is open.

`reset()` returns to the attach state (off with a PWRKEY, booting without)
and clears every counter, every connection and the read-only regions; the
rw properties and the rw regions keep what the host set. `reset(True)`
restores their defaults too, including the part's default baud rate.
There are no faults.

## Regions

| name | part | size | access | contents |
| --- | --- | --- | --- | --- |
| `hosts` | network | 192 | rw | the fake DNS: text lines `name=a.b.c.d`, matched without case; a dotted IPv4 literal needs no entry |
| `file` | network | 1024 | rw | the body the HTTP and FTP servers serve, the first `file_length` bytes |
| `request` | network | 96 | ro | the first line of the last HTTP request, or `FTP <path><name>` |
| `mqtt_last` | network | 96 | ro | the last `PUBLISH` the broker received: `topic\npayload` |
| `mqtt_inject` | network | 96 | rw | what `mqtt_send` publishes: `topic\npayload` |
| `sms_inbox` | SMS | 384 | rw | stored SMS, one PDU in hex per line; the line number is the index from 1 |
| `sms_outbox` | SMS | 192 | ro | the last message sent: `number\ntext` in text mode, the PDU hex in PDU mode |

Text regions are NUL padded and read up to the first NUL, so a shorter
write needs its own terminating NUL. A message deleted with `CMGD` stays
deleted across reboots, as on the SIM; writing `sms_inbox` or `reset()`
undeletes every message.

## The engine

All timed behaviour runs on the simulation clock in `time_advanced`: the
PWRKEY is sampled on every call, and the end of boot and due URCs are
delivered. What the part has already decided to send leaves in real time
from `poll`, also while the clock is paused: the output FIFO is pumped,
the link adds PPP and CMUX frames as the engine makes room, and a due
`AT+IPR` switch happens once its `OK` is out. The model is meant to be
used with the clock running.

| from | event | to |
| --- | --- | --- |
| off | PWRKEY low for the part's Ton | booting |
| booting or on | PWRKEY low for the part's Toff | the power-down message, off |
| booting or on | RESET_N low for the part's reset pulse | off (held in reset), output not yet sent lost |
| off, held in reset | RESET_N released | booting |
| booting | `boot_ms` later | the part's boot URCs; on |

The pin is sampled once a millisecond, so a pulse is seen when it spans a
sample. A press does one thing and the key must be released before the next one
counts; a key already low at attach or reset counts as used, so a DUT pin
floating low before its driver runs is not a press, and a RESET_N already
low is not a reset until it has been high. Off, every UART byte
is ignored and nothing is sent; booting likewise. A boot starts with echo
on, `CMEE` 0, `CFUN` 1, the bearer down, every socket closed and GNSS off;
the baud rate survives.

A line ends at CR; an LF directly after that CR is dropped, even when the
line opened a data phase, so the LF of a `...=<len>\r\n` command is never
the first payload byte. The flag is kept across UART chunks: a DUT that
ends the line with CR only and starts its payload with LF loses that LF.
Bytes before an `A` (LF, spaces, NUL) are noise. The `AT` prefix is
case-insensitive, the command name is matched without case, and one
command per line is taken (no `;` concatenation). A model's tables take
extended commands with `+` and vendor ones with `^` (`AT^RESET`), and
`ATD<string>` when one maps `D`. With echo on the line comes back as
received followed by CR, then the answer. Answers use V.250 framing:
`\r\n<info>\r\n`, a multi-line list opens with one CR LF and ends every
line with one, and the final result is `\r\nOK\r\n`. `ATV0` switches the
channel to numeric result codes: `0\r` for OK, `4\r` for ERROR, `1\r`
for CONNECT, `3\r` for NO CARRIER, and information responses without
their leading CR LF; `ATV1` switches back. An error in a modelled command
is `ERROR` with `CMEE` 0, `+CME ERROR: <n>` with `CMEE` 1 and the 27.007
text with `CMEE` 2 (`+CMS ERROR` for SMS); an unknown command or a
malformed one is always `ERROR`.

`+CREG` reports the circuit domain and `+CGREG` and `+CEREG` the packet
domain (27.007 7.2, 10.1.19): after `AT+CGATT=0` the latter two read and
report stat 0 while `+CREG` keeps the network's, and the modem is no
longer attached.

All output goes through one 704-byte FIFO pumped into the UART engine's
512-byte ring. A URC waits until no command line is half received, no
data phase is open, no final result is pending, no baud switch is due and
the FIFO has room for the part's longest URC, then goes out in the order
it was raised; so a URC never lands inside a response, between `>` and its
`OK`, or inside socket data. `AT+IPR` switches the rate once its `OK` has
left at the old one.

URCs are coalesced, so a burst never overflows the queue: one pending
registration URC carries the latest stat, a per-connection URC waits once
per connection, and any other kind is replaced by its newer instance.

Commands are taken one at a time. Input that arrives while a deferred `OK`
is pending, or while the output FIFO has less room than the part's longest
answer, is held (up to 48 bytes) and run once the modem can answer, so
pipelined commands are answered in order; anything past those 48 bytes is
lost.

## CMUX and PPP

A model with a link answers Zephyr's `modem_cellular` the way its part
does: `AT+CMUX`, then 3GPP TS 27.010 on the UART, `ATD*99***1#` on DLCI 1,
then PPP, and Zephyr's own IP stack talking to the fake network.

Before CMUX the raw UART is one AT channel. `AT+CMUX=0[,0[,<speed>[,<N1>]]]`
answers `OK` there and starts the multiplexer; N1 (default 31) is clamped
to 127, Zephyr's MTU, and bounds every frame the model sends. The model is
the responder:

- DLCI 0: SABM gets UA; an MSC command gets its response and records the
  DUT's flow control for that DLCI (FC set or RTR clear stops it);
  FCon and FCoff, Test and PSC get their responses; any other command
  gets NSC. CLD gets its response, and DISC on DLCI 0 its UA; either ends
  the multiplexer and the raw UART takes AT again at the same rate.
- DLCIs 1 to 4: SABM gets UA (C/R 1) and opens an AT channel; DLCI 1 is
  the raw channel's successor, DLCI 2 and one of 3 and 4 get channels of
  their own, and a SABM with no channel left, or on DLCI 5 or above, gets
  DM. DISC gets UA and closes the channel, ending PPP if it carried it.
- A frame with a bad FCS is dropped and counted; any run of flags
  resynchronises. Every frame the model sends has its own flags, never
  more than one in a row.

Each channel has its own line, echo, `ATV`, data phase and deferred
result, and starts with the raw UART's `ATE` and `ATV`; a handler answers
on the channel its command came from. A URC goes to every channel in
command mode once all of them are idle, never to one carrying PPP.
Channel output collects up to N1 bytes and goes out as UIH frames (C/R 0).

`ATD*99#` or `ATD*99***<cid>#` on any channel answers
`CONNECT <baud>` while the modem is attached, `NO CARRIER` otherwise, and
turns the channel to PPP for context `<cid>` (1 for `ATD*99#`). One channel
carries PPP at a time.

PPP follows RFC 1661 and 1662 as Zephyr's `modem_ppp` speaks them. The
model is passive: it sends nothing until the DUT's first LCP packet, then
acknowledges the DUT's Configure-Request with an exact copy (MRU, ACCM
and Magic-Number accepted, anything else rejected) and sends its own: MRU
`mru`, ACCM 0. Every frame carries a correct FCS-16 and the control field
escaped; until LCP opens every control character is escaped, then those
the DUT's ACCM names. Received frames are checked against their FCS. IPCP
rejects every option but the address and the first DNS server (RFC 1877),
NAKs those with the network's `ip` and `dns`, and acknowledges the request
that carries them; its own request is `gw`. LCP Echo-Request gets
Echo-Reply, an unknown protocol gets Protocol-Reject, and the restart
timer is 3 s for 10 tries. When the DUT sends Terminate-Request the model
acknowledges it; when the network goes with the link up the model sends
Terminate-Request (twice, 3 s apart). Either way, once the exchange is
over the channel answers `NO CARRIER` and takes AT again, and the DUT
redials.

Behind PPP every address is the internet, as for the AT sockets, and the
same services answer (see the fake network below):

- IPv4 without options or fragments; every checksum is checked and sent
  right. Replies come from the address the DUT sent to.
- ICMP echo requests are answered.
- UDP: port 53 is a DNS server for `hosts` (an A question gets its answer
  with a 60 s TTL, a miss NXDOMAIN), 7 echoes, 9 discards; anything else
  is dropped and counted in `drops`. One reply waits at a time; a
  datagram that finds it taken is dropped and counted.
- TCP: up to four connections, each on a closed socket of the network.
  A SYN to a refused port gets RST|ACK. The SYN-ACK carries the only MSS
  option, the smaller of the DUT's and `mru` - 40, and a window of what
  the service takes, at most two segments. Every data segment is
  acknowledged at once, on outgoing data when there is some; out-of-order
  data is dropped with a duplicate ACK. At most two segments are in
  flight, each with PSH, never into a zero window, which is probed after
  2 s. The model's RTO is 1 s, doubling, six tries, then RST. The model
  sends FIN once the service has closed and the DUT has acknowledged
  everything, or at once after the DUT's FIN; a connection the model
  closed first stays 2 s to acknowledge a repeated FIN.

Everything runs on the simulation clock with no randomness: ISNs and IP
IDs come from counters.

The link adds a PPP frame only while the UART bytes queued in the FIFO and
in flight in the UART engine stay under 256, and never on a DLCI the DUT
has flow-stopped. With `mru` 296 a segment takes about 29 ms at 115200, so
the model's ACKs reach the DUT well inside Zephyr's fixed 200 ms RTO, and
an AT answer on DLCI 2 waits behind at most 22 ms of PPP.

A DUT on `modem_cellular` needs `CONFIG_MODEM_CMUX_WORK_BUFFER_SIZE_EXTRA`
of 512 or so: with the default DLCI buffers Zephyr drops two back-to-back
full frames that arrive in one receive batch.

## The fake network

The service is chosen by the destination port, whatever address the host
resolved to.

| port | TCP | UDP |
| --- | --- | --- |
| 7 | echo | echo |
| 9 | discard | discard |
| 80, 443, 8080 | HTTP | discard |
| 1883 | MQTT | discard |
| anything else | refused | discard |

Every socket has a 256-byte receive buffer; what does not fit is counted in
`drops`. A server close (the end of an HTTP response, an MQTT `DISCONNECT`)
happens only once the DUT has read every byte; the socket then stays
closed by the server until the DUT closes it. Losing the network ends
every connection at once, its pending data discarded.

HTTP collects the request until its blank line and stores its first line
in `request`; when requests on two connections overlap, the one that started
last owns the region. The response is generated as it is read:
`HTTP/1.1 200 OK`, `Content-Type: application/octet-stream`,
`Content-Length: <file_length>` and `Connection: close`, then the first
`file_length` bytes of `file`; with `file_length` 0 it is
`HTTP/1.1 404 Not Found` with length 0. The length is taken when the
request completes. Request bodies are ignored and one request is served per
connection.

The MQTT broker parses MQTT 3.1.1 out of the upstream bytes, packets
spanning sends included. `CONNECT` gets `CONNACK` (accepted), `SUBSCRIBE`
gets `SUBACK` granting QoS 0 for up to two exact topic filters per
connection and refusing the rest (0x80), `UNSUBSCRIBE` gets `UNSUBACK`,
`PINGREQ` gets `PINGRESP`, a QoS 1 `PUBLISH` gets `PUBACK`, and
`DISCONNECT` closes. A `PUBLISH` is stored in `mqtt_last`, counted in
`mqtt_publishes`, and forwarded at QoS 0 to every connection subscribed to
its topic, its own included, when topic and payload fit in 95 bytes.
Setting `mqtt_send` forwards `mqtt_inject` the same way. Wildcard topic
filters do not match.

The FTP server serves `file` too; the names a model's FTP commands set are
recorded in `request` as `FTP <path><name>`.

## Writing a model

A model is a `<part>.c` (and `<part>.h`) in this package, a
`metalclaw_device()` line in `device.cmake` listing the engine sources and
its own, a `test/cases/test_<part>.py` and a `test/dut/<part>/` image. Its
state struct starts with `struct modem` and holds the components it has
(`struct net`, `struct sms`, `struct gnss`), its URC queue and its socket
array. Its `init` points `m.net`, `m.sms` and `m.gnss` at them, calls
`net_init()` with the sockets and its notice handler, and calls
`modem_init()` with the queue; every other op is the engine's. Its
`struct modem_config`, through `model->config`, carries the identity
strings, the IPR rates, the CFUN levels, the power pin (PWRKEY timing, or
`power_pin` `MODEM_PIN_RESET` with its `reset_us` and
`MODEM_BINDINGS_RESET`) and boot time, the power-down line, the FIFO room
for its longest answer and URC, its command tables (its own first, then
the shared ones it answers), its parts (in table order), and four hooks:
the boot URCs, its URC kinds, the bearer lost with the network, and the
+COPS access technology. `modem.h` and `net.h` state the contract of each.

A model with CMUX and PPP also holds a `struct modem_link` (about 2.7 KB)
and at least four sockets for its TCP connections, points `m.link` at it
before `modem_init()`, maps `+CMUX` to `modem_cmd_cmux` and `D` to
`modem_cmd_dial_ppp` in its table, lists `ppp_part` with
`PPP_PROPERTIES`, and calls `modem_link_bearer_lost()` from its
`bearer_lost` hook. The link raises and drops `net->ses.bearer` with IPCP
unless the model's optional `link_bearer` hook takes that decision for the
dialled context, as `a7672x` and `sequans_monarch` do, whose `CGACT` (and
`NETOPEN`, `CCHSTART`) share the bearer: `CGACT?` shows the context the
call holds, and the call ending leaves a context those commands raised up.
The hook keeps `net->ses.bearer` true while PPP is up, so a lost network
reaches `bearer_lost`, which the engine calls only with the bearer up. A model whose sockets are
the link's alone can pass `modem_link_net_notify` to `net_init()`; one
with AT sockets as well leaves alone the notices of a socket
`modem_link_owns_socket()` names.

A model translates its AT dialect to `net_open`, `net_send`,
`net_available`, `net_read` and `net_close` and turns the network's
notices into its URCs. The network knows no AT, so an IP stack behind PPP
can drive the same sockets and services.

## Tests

`test/cases/common.py` holds what every model's cases share: the attach
line for a model id from the fixture's `uart` and `gpio`, the
`Modem` helper for properties and regions over the REPL, the hosts table
and file the network serves, the DUT's echo command, and SMS-DELIVER PDUs
for the inbox. A model's cases import it relatively
(`from .common import ...`) and skip unless the DUT image is
`cellular/<part>`, built with `just build-dut cellular/<part>`.

`common.py` also holds the cases every model behind Zephyr's
`modem_cellular` shares, `PppModelCases`: a model's file subclasses it with
the modem info, registration and RSRP the driver reports for that model and
whether the driver's power-off leaves the part powered. They attach the
model by its own pin binding (`pwrkey` or `reset`, read from the schema;
the fixture's `gpio` names GP6 both ways), bring the link up with the
DUT's `NET UP`, and check CMUX and PPP through the model's `cmux` and
`ppp_up`, the cellular API through `CELLULAR`, DNS, TCP, UDP and HTTP on
Zephyr's IP stack, a short and a long loss of registration, and power off
and on. `Uart` is the DUT's `UART` command, for a model no Zephyr driver
speaks: AT lines, raw data phases and the URCs between them; `gpio`
presses its PWRKEY.
