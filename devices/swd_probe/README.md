<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->

# swd_probe

A CMSIS-DAP v2 debug probe. It is a device in the Metalclaw sense: the DUT
finds it on the other side of the SWD pins, and a user names it in `attach()`.
Attaching it claims the SWD wire and adds a vendor bulk interface to the
harness's USB device; a host debugger then drives the DUT through it.

The package is two files. `dap.c` is the portable CMSIS-DAP command processor,
ported from the old repository's `src/probe/`; it speaks only
`metalclaw/hal/swd.h`. `swd_probe.c` is the model: descriptors, properties and
the ops the runtime calls.

## Bindings

| index | name | kind | required | resolved into |
| --- | --- | --- | --- | --- |
| 0 | `swd` | `swd_wire` | yes | the wire, from a pin tuple `(swclk, swdio[, nreset])` |
| 1 | `dap` | `usb_interface` | yes | a vendor interface, class `0xFF`, string `CMSIS-DAP v2` |

`dap` takes no `attach()` keyword; the runtime allocates the interface from the
descriptor the model carries. probe-rs recognises a v2 probe by that interface
string, so it must not change.

On fixture 1 that is SWCLK on GP2, SWDIO on GP3 and the DUT's RUN line on GP28:

```python
import metalclaw
probe = metalclaw.attach("swd_probe", "probe", swd=(2, 3, 28))
probe.clock_hz = 1000000
probe.connected, probe.transfers, probe.errors
probe.detach()
```

## Properties

| name | type | access | unit | range | meaning |
| --- | --- | --- | --- | --- | --- |
| `enabled` | bool | rw | | | the wire is powered and DAP traffic is answered |
| `clock_hz` | u32 | rw | Hz | 50 000 .. 10 000 000 | SWD clock; reads back from the HAL |
| `connected` | bool | ro | | | true between `DAP_Connect` and `DAP_Disconnect` |
| `transfers` | u32 | ro | | | words completed by `DAP_Transfer` / `DAP_TransferBlock` |
| `errors` | u32 | ro | | | wire errors: failed transfers, non-OK ACKs, failed sequences |
| `short_replies` | u32 | ro | | | DAP replies the USB interface did not take whole, its queue and backlog full; the rest of each is dropped |

`enabled` defaults to true, so `attach()` turns the port on. Setting it false
turns the port off, drops `connected`, and makes every wire-touching DAP
command answer with an error; `DAP_Info` and `DAP_HostStatus` still answer, so
the probe stays enumerable and identifiable while parked.

`reset()` disconnects, clears the counters, and puts the transfer
configuration back to its defaults (no idle cycles, no WAIT retries,
turnaround 1, no data phase). `reset(factory=True)` also puts the clock back
to 50 kHz.

There are no regions and no faults.

## What a host sees

The harness enumerates as `2FE3:0204` with the RP2040's unique id as its USB
serial, so the probe's identity is `2fe3:0204:<serial>` — on this bench
`2fe3:0204:E6612483CB313225`.

```sh
probe-rs list
probe-rs info --probe 2fe3:0204:<serial>
```

`DAP_Info` reports vendor `Metalclaw`, product `Metalclaw`, the USB serial,
the firmware version string, SWD-only capabilities, a 64-byte packet size and
one packet in flight. Target and board strings are reported empty: the probe
does not know what is on the other end of the wire.

Commands: `DAP_Info`, `DAP_HostStatus`, `DAP_Connect`, `DAP_Disconnect`,
`DAP_TransferConfigure`, `DAP_Transfer`, `DAP_TransferBlock`,
`DAP_TransferAbort`, `DAP_WriteABORT`, `DAP_Delay`, `DAP_ResetTarget`,
`DAP_SWJ_Pins`, `DAP_SWJ_Clock`, `DAP_SWJ_Sequence`, `DAP_SWD_Configure`,
`DAP_SWD_Sequence`. Anything else answers `0xFF`. There is no JTAG and no SWO.

Transfer requests that ask for a value match, a match mask or a timestamp are
answered with an error; the old core did the same, and no host on this bench
has needed them.

A packet is processed to completion inside the model's `usb_rx` op and its
response is handed to the interface before the op returns, so requests are
never reordered and the model keeps no queue. The interface holds up to 512
bytes the host has not read yet, and a further 512 that wait in order until
it drains. Only when the host has left both full does a response not fit;
the part that does not fit is dropped and `short_replies` counts it, so the
host sees a short or missing reply.

## Tests

`test/cases.py` runs on the rig's `harness`, `dut` and `probe` fixtures. The
rig's hardware platform attaches this device itself, on the pins its fixture
description gives as `swd`, because every other case reaches the DUT through
it. The cases check that the instance is there and that the harness advertises
its CMSIS-DAP interface on USB, that probe-rs and the rig's own DAP client
(`test/rig/platforms/hardware/dap.py`) both read the DUT's `CPUID` through
it, and that such a read moves the model's `transfers` counter. probe-rs
halts the DUT while its session attaches and detaches; the DAP client goes
straight to the memory access port and does not. On the `smoke` image, one
case downloads the image through the probe with `probe-rs download --verify`,
resets the DUT and waits for it to answer PING.

`test/unit/test_swd_probe.py`, at the repository root, builds the model on the
host with the strict warning set, against a fake HAL whose vendor write takes
only part of a reply, and checks `short_replies`: a reply taken whole leaves
it alone, one cut short or refused counts once, and `reset()` clears it.
