<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# ds18b20

A Maxim DS18B20 digital thermometer on 1-Wire, externally powered:
`ds18b20`. The harness's `wire` binding runs the `onewire` engine: reset
and presence and every time slot in a state machine, and, once a slot, the
1-Wire network layer in core (`metalclaw/onewire.h`): the ROM commands and
the function commands this model's image describes. The model computes
conversions on the simulation clock.

## Datasheet

Maxim DS18B20 data sheet REV 042208 (`DS18B20_maxim_042208.pdf`). Figure,
table and section names below are from it.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `dq` | `wire` (`onewire`) | yes | DQ, one pin, open drain |

The bus needs its pull-up (Figure 10, 5 kΩ): the harness pin only pulls low
or lets go. On a bench without line pull-ups the DUT pad's own pull-down,
which `gpio_init` leaves on, holds the bus near half the supply against the
harness's 50-80 kΩ pull-up, so the part is usable only where the bench pulls
its lines up (the MetalClaw Pico RP2's 5.1 kΩ).

```python
import metalclaw
t = metalclaw.attach("ds18b20", "t", dq=24)
t.temperature = 21375          # mC
t.serial = 0xC0FFEE
t.rom                          # (low, high) words of the 64-bit ROM code
t.conversions, t.searches, t.matches, t.scratchpad_reads, t.resets
t.read_region("scratchpad", 0, 9)
```

## Properties

| name | type | access | unit | meaning |
| --- | --- | --- | --- | --- |
| `temperature` | i32 rw | mC | -55 000 .. 125 000 | what the next conversion measures; 25 000 at attach |
| `serial` | u32 rw | | | the ROM's serial number, bits 31..0 (bits 47..32 are 0) |
| `rom` | u32 × 2 ro | | | the ROM code, family code in the lowest byte |
| `resolution` | u32 ro | bits | 9 .. 12 | from the configuration register |
| `th`, `tl` | i32 ro | °C | | the alarm triggers in the scratchpad |
| `alarm` | bool ro | | | the alarm flag |
| `conversions`, `scratchpad_reads`, `searches`, `matches`, `resets` | u32 ro | | | what the master did |

## Regions

| name | size | access | content |
| --- | --- | --- | --- |
| `scratchpad` | 9 | ro | the scratchpad (Figure 7), CRC included |
| `rom` | 8 | ro | the ROM code (Figure 6) |

## Behaviour

- **Signalling** (Figures 13 and 14): a low longer than 184 µs is a reset
  (longer than tLOW0's 120 µs, shorter than tRSTL's 480 µs); 30 µs after its
  rising edge the part pulls the bus low for 120 µs (tPDHIGH 15-60 µs,
  tPDLOW 60-240 µs). The part samples a write slot 30 µs after its falling
  edge (the typical point in Figure 14) and, to send a 0, holds the bus low
  from about 2 µs to 30 µs. What the part sends in a slot is decided at the
  slot before; the slot handler has until 55 µs into a slot to answer.
- **ROM commands** (Figure 11): SEARCH ROM F0h, READ ROM 33h, MATCH ROM 55h,
  SKIP ROM CCh, and ALARM SEARCH ECh, answered only while the alarm flag is
  set. A part not matched waits for the next reset.
- **ROM code** (Figure 6): family 28h, the 48-bit serial number, the CRC.
- **Function commands** (Table 3): Convert T 44h answers read slots with 0
  until the conversion is done, then 1, the done showing one slot late at
  most. A conversion takes Table 2's maximum for the resolution (93.75 ms at
  9 bits to 750 ms at 12) on the simulation clock, rounds the temperature
  to the nearest 1/16 °C, clears the bits the resolution leaves undefined
  (Figure 2), and sets the alarm when bits 11..4 are at or above TH or at or
  below TL. Read Scratchpad BEh sends up to nine bytes. Write Scratchpad 4Eh
  takes TH, TL and the configuration register, whose bits other than R1 and
  R0 keep their fixed values (Figure 8); the model applies it at its next
  poll, within a millisecond. Copy Scratchpad 48h stores TH, TL and the
  configuration in the EEPROM; Recall E² B8h loads them, answering read
  slots like Convert T. Read Power Supply B4h leaves the bus high: the
  part is externally powered.
- **CRC** (Figure 9): X⁸ + X⁵ + X⁴ + 1 over the ROM's first seven bytes and
  the scratchpad's first eight.
- **Power-up** (Figure 7): the temperature register holds +85 °C (0550h),
  reserved bytes FFh, FFh and 10h (the datasheet gives no value for byte 6),
  and the EEPROM's TH, TL and configuration. The datasheet leaves the EEPROM
  to the user; the model starts it at TH 125 °C, TL -55 °C, 12 bits.

## Not modelled

- Parasite power and the strong pull-up it needs; the part never pulls the
  bus low in Read Power Supply.
- Overdrive speed.
- EEPROM write time (tWR 10 ms): Copy Scratchpad completes at once.

## Timing on the harness

The engine is one state machine at 1 MHz (27 instruction words) and a DMA
channel whose completion interrupt, on DMA_IRQ_1 on the worker core, runs
the network layer once a slot. Times are judged to the state machine's
1 µs, against windows of 15 µs and more.
