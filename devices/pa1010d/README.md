<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# pa1010d

A CDTop PA1010D GNSS module, MediaTek MT3333 inside, on its I2C port. The
host sets a position, speed, course, fix and UTC time; every fix interval
of the simulation clock the model writes that fix as the NMEA 0183
sentences the part is set to output, and the DUT reads them out of the
module's I2C buffer and writes PMTK commands into it.

The model has no bus code. Its image is the I2C buffer: a register map with
no register pointer, which the I2C engine serves from address 0 on every
read and whose written bytes it hands to the model's `on_input` hook
(`mclw_regmap_set_on_input`). The model parses PMTK packets from those
bytes on the worker and answers into the NMEA it queues.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `i2c` | `i2c_target` | yes | an I2C target, from the tuple `(sda, scl, address)` |

The module answers at 0x10, up to 400 kHz (GlobalTop "NMEA over I2C" guide,
2). The UART the module also has is not modelled.

```python
import metalclaw
gnss = metalclaw.attach("pa1010d", "gnss", i2c=(4, 5, 0x10))
gnss.set(latitude=473977420, longitude=-1223321500, speed=1500, heading=9000000,
         satellites=8, fix=3, time=(2026, 11, 15, 12, 34, 56))
gnss.nmea_rates, gnss.rate_ms, gnss.gnss, gnss.commands, gnss.packets
gnss.reset()
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `latitude`, `longitude` | i32 | rw | 1e-7 deg | sent as ddmm.mmmm and dddmm.mmmm |
| `altitude` | i32 | rw | mm | GGA's MSL altitude to 0.1 m; geoidal separation 0 |
| `speed` | u32 | rw | mm/s | RMC and VTG in knots, VTG in km/h, to 0.01 |
| `heading` | i32 | rw | 1e-5 deg | RMC and VTG course to 0.01 deg |
| `satellites` | u32 | rw | | 0 .. 32, split between GPS and GLONASS when both are searched |
| `fix` | u32 | rw | | 0 none, 2 2D, 3 3D (GSA mode 2); 1 reads as none |
| `pdop`, `hdop`, `vdop` | u32 | rw | 0.01 | GSA's DOPs, GGA's HDOP |
| `time` | i32[6] | rw | | UTC year .. second; a set is the time the next fix carries, and it runs on with the simulation clock |
| `gnss` | u32 | rw | | bit 0 GPS, bit 1 GLONASS, as `$PMTK353` left them |
| `rate_ms` | u32 | ro | ms | the fix interval `$PMTK220` set |
| `nmea_rates` | u32[6] | ro | | GLL, RMC, VTG, GGA, GSA, GSV every n fixes, 0 off: `$PMTK314`'s first six fields |
| `commands` | u32 | ro | | PMTK packets taken |
| `sentences` | u32 | ro | | NMEA sentences queued |
| `dropped` | u32 | ro | | sentences a new fix replaced unread, or that did not fit |
| `packets` | u32 | ro | | I2C reads the master has taken |

At power-on: no fix, 1980-01-06 00:00:00, PDOP 2.50, HDOP 1.50, VDOP 2.00,
GPS and GLONASS, 1 Hz (datasheet 2.6), and GGA, GSA, GSV, RMC and VTG every
fix, the five sentences datasheet Table-1 lists, GLL off. Without backup
power the module forgets every setting (PMTK command packet Rev.A11, note
after PMTK 010), so `reset()` is a power cycle back to all of that. There
are no regions and no faults.

## The I2C buffer

GlobalTop's "NMEA over I2C Software Guide" 3 to 3.2: the module's I2C TX
buffer holds a packet of at most 254 NMEA bytes and an end character,
`<LF>`, and the master reads it as 255 bytes; past the NMEA every byte is
garbage, `<LF>` (0x0A). A packet read, the module puts up the next part of
the second's NMEA, and the master is told to wait 2 ms for it. A new fix's
sentences replace whatever of the last the master had not read.

The model keeps the fix's output in a queue and the image holds its first
254 bytes, then `<LF>` to the end of the 256-byte image. Every read starts
at the packet's first byte. Once no byte has crossed the bus for a
millisecond, the bytes read since the packet went up, at most the packet's
NMEA, leave the queue, and the next packet goes up. Reads closer together
than that read the same packet again. Each fix empties the queue first,
the unread sentences counted in `dropped`.

Written bytes are PMTK input. Section 4 of the guide asks for 10 ms
between written packets; the model takes input however fast it comes.

## PMTK

Packets are `$PMTK`, a three-digit type, comma-separated fields, `*`, the
XOR of everything between `$` and `*` in two hex digits, CR LF (Rev.A11,
"Packet Format"). A packet with a bad checksum is ignored. Replies go into
the NMEA queue.

| packet | effect | reply |
| --- | --- | --- |
| `000` test | | `$PMTK001,000,3` |
| `101` .. `103` hot, warm, cold start | the fix count restarts | `$PMTK010,001`, `$PMTK011,MTKGPS` |
| `104` full cold start | also back to the factory settings | the same |
| `220,<ms>` | fix interval, 100 .. 10000 | `$PMTK001,220,3`, or `,2` out of range |
| `314,<f0>..<f18>` | sentence rates, each 0 .. 5; fields left out are off; `-1` alone restores the default | `$PMTK001,314,3`, `,2` for a bad field, `,0` for none or more than 19 |
| `353,<gps>,<glonass>[,...]` | the GNSS searched (datasheet notes to Table-3, 10); the fields after the second must be 0 | `$PMTK001,353,3`, or `,2` |
| `414` | | `$PMTK514` with the 19 rates |
| any other type | | `$PMTK001,<type>,1`, unsupported |

## NMEA output

Formats from the datasheet's examples and tables (3.1): GGA (Table-3),
GSA (Table-5), GSV (Table-8, 9), RMC (Table-10), VTG; GLL, which 314 can
turn on, carries position, time, status and mode. Times are hhmmss.sss,
positions ddmm.mmmm. The talker is `GP` or `GL` with one system searched
and `GN` for GGA, RMC, VTG and GLL with both; GSA and GSV carry each
system's own (Table-2). An epoch goes out in the tables' order: GGA, GSA
per system, GSV per system and page, RMC, VTG, then GLL. RMC leaves the
magnetic variation empty, as the datasheet's second example does. Without a
fix the time still runs, the position fields are empty, GGA's fix
indicator is 0, GSA's mode 1, RMC's status V and the mode indicator N. The
satellites are synthetic, GPS PRNs from 1 and GLONASS from 65, each with
its own elevation, azimuth and SNR.

Not modelled: the UART, the interrupt pin GIO8, 1PPS, PMTK packets other
than those above (EPO, standby, periodic and AlwaysLocate modes, the
logger, SBAS, DGPS, datum, baud rate), Galileo and QZSS, ZDA and the other
MTK sentences, the startup messages at power-on, and the time a real
restart takes.

## pico-examples

`i2c/pa1010d_i2c` (pico-examples 0d62f75) writes `$PMTK314` (17 fields,
RMC alone) one byte per transaction, then each second reads 250 bytes and
prints the fields of the `$GNRMC` in them. Its first read comes before the
part has taken the command and holds the start of a full epoch, with no RMC
in its first 254 bytes, so the program's `strstr` finds none and it prints
from address 0; every later read holds one RMC.

## Sources

CD-PA1010D Data Sheet V.03; GlobalTop "GPS NMEA over I2C Software Guide"
v1.0 (2014-10-01, for the MT3339 and MT3333); GlobalTop "PMTK command
packet" Rev.A11. The sentence formats follow the datasheet.
