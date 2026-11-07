# SPDX-License-Identifier: Apache-2.0 OR MIT
# DUT memory over the harness's CMSIS-DAP v2 interface, without a debug
# session. Every probe-rs command opens a session, and a session halts every
# core twice (probe-rs 0.32 `Session::new` and its drop clear the hardware
# breakpoints under `halted_access`), about 13 ms each time on this RP2350.
# A halted DUT cannot drain its 32-byte UART FIFO, so a mailbox poll that
# lands on a modem's reply loses the bytes. This talks to the MEM-AP directly:
# the core keeps running, DHCSR is never written.

import os
import struct
import time
from dataclasses import dataclass

import usb.core
import usb.util

VENDOR_ID = 0x2FE3
PRODUCT_ID = 0x0204
INTERFACE_NAME = "CMSIS-DAP v2"
PACKET = 64
USB_TIMEOUT_MS = 1000
# The variable patched probe-rs reads, so one setting covers both clients.
USB_TIMEOUT_ENV = "CMSIS_DAP_USB_TIMEOUT_MS"
DRAIN_TIMEOUT_MS = 20

DAP_CONNECT = 0x02
DAP_DISCONNECT = 0x03
DAP_TRANSFER_CONFIGURE = 0x04
DAP_TRANSFER = 0x05
DAP_TRANSFER_BLOCK = 0x06
DAP_SWJ_CLOCK = 0x11
DAP_SWJ_SEQUENCE = 0x12
DAP_SWD_CONFIGURE = 0x13
DAP_SWD_SEQUENCE = 0x1D
DAP_OK = 0x00
PORT_SWD = 0x01

REQUEST_AP = 0x01
REQUEST_READ = 0x02
ACK_OK = 0x01

DP_DPIDR = 0x0
DP_ABORT = 0x0
DP_CTRL_STAT = 0x4
DP_SELECT = 0x8
DP_RDBUFF = 0xC
ABORT_CLEAR_ALL = 0x1E
POWER_UP_REQUEST = 0x50000000
POWER_UP_ACK = 0xA0000000

AP_CSW = 0x00
AP_TAR = 0x04
AP_DRW = 0x0C
CSW_SIZE_MASK = 0x7
CSW_SIZE_32 = 0x2
CSW_ADDRINC_MASK = 0x30
CSW_ADDRINC_SINGLE = 0x10
# TAR auto-increment is only guaranteed within a 1 KiB block.
TAR_WRAP = 0x400

# SWJ sequences, sent LSB first (ADIv6 B5.3). The JTAG-to-SWD switch is what
# probe-rs uses on this DP; the dormant wake-up is the fallback for a DP that
# was put to sleep.
LINE_RESET = (51, b"\xff" * 7)
JTAG_TO_SWD = (16, bytes([0x9E, 0xE7]))
IDLE = (3, b"\x00")
SWD_TO_DORMANT = (16, bytes([0xBC, 0xE3]))
WAKE_CYCLES = (8, b"\xff")
SELECTION_ALERT = (128, bytes([
    0x92, 0xF3, 0x09, 0x62, 0x95, 0x2D, 0x85, 0x86,
    0xE9, 0xAF, 0xDD, 0xE3, 0xA2, 0x0E, 0xBC, 0x19,
]))
# Four low cycles, then the SWD activation code 0x1A.
ACTIVATE_SWD = (12, bytes([0xA0, 0x01]))
DP_SELECT1_BANK = 0x5
# A TARGETSEL write: the request, five undriven cycles where the ACK would
# be (no target answers it), then 32 data bits and parity.
TARGETSEL_REQUEST = 0x99
TARGETSEL_ACK_CYCLES = 0x80 | 5


@dataclass(frozen=True)
class DebugPort:
    """How the probe reaches core 0's AHB MEM-AP on a DUT chip."""

    # SELECT for the MEM-AP's register bank 0.
    ap_select: int
    # The multidrop target to select after every line reset; None for a
    # point-to-point DP.
    targetsel: int | None = None


# RP2350 is an ADIv6 target: an AP register is addressed by its full offset,
# the 4 KiB-aligned AP base plus the register, through SELECT. Core 0's
# AHB MEM-AP sits at 0x2000 on fixture 1's Pico 2 (see rp2350_core0.yaml).
RP2350 = DebugPort(ap_select=0x2D00)
# RP2040 is ADIv5 on an SWD multidrop bus: core 0's DP is TARGETSEL
# 0x01002927 and its MEM-AP is APSEL 0.
RP2040 = DebugPort(ap_select=0x0, targetsel=0x01002927)


class DapError(RuntimeError):
    pass


def usb_timeout_ms():
    value = os.environ.get(USB_TIMEOUT_ENV, "").strip()
    if not value:
        return USB_TIMEOUT_MS
    try:
        milliseconds = int(value)
    except ValueError:
        milliseconds = 0
    if milliseconds <= 0:
        raise DapError(f"invalid {USB_TIMEOUT_ENV} {value!r}; expected positive milliseconds")
    return milliseconds


class DapMemory:
    def __init__(self, serial, debug_port=RP2350, clock_hz=4_000_000):
        self.serial = serial
        self.debug_port = debug_port
        self.clock_hz = int(clock_hz)

    def read_words(self, address, count, timeout=None):
        if address <= 0 or address % 4 or count <= 0:
            raise DapError(f"invalid read of {count} words at {address:#x}")
        with _Link(self.serial, self.clock_hz, self.debug_port) as link:
            return link.read_words(address, count)

    def write_words(self, address, words, timeout=None):
        values = [value & 0xFFFFFFFF for value in words]
        if address <= 0 or address % 4 or not values:
            raise DapError(f"invalid write of {len(values)} words at {address:#x}")
        with _Link(self.serial, self.clock_hz, self.debug_port) as link:
            link.write_words(address, values)


class _Link:
    def __init__(self, serial, clock_hz, debug_port):
        self.serial = serial
        self.clock_hz = clock_hz
        self.port = debug_port
        self.device = None
        self.interface = None
        self.out_ep = None
        self.in_ep = None
        self.select = None
        self.timeout_ms = usb_timeout_ms()

    def __enter__(self):
        device = usb.core.find(idVendor=VENDOR_ID, idProduct=PRODUCT_ID, serial_number=self.serial)
        if device is None:
            raise DapError(f"no harness {self.serial} on USB")
        for interface in device.get_active_configuration():
            if interface.iInterface and usb.util.get_string(device, interface.iInterface) == INTERFACE_NAME:
                self.interface = interface
                break
        if self.interface is None:
            raise DapError(f"harness {self.serial} has no {INTERFACE_NAME} interface")
        self.device = device
        usb.util.claim_interface(device, self.interface.bInterfaceNumber)
        for endpoint in self.interface:
            if usb.util.endpoint_direction(endpoint.bEndpointAddress) == usb.util.ENDPOINT_OUT:
                self.out_ep = endpoint
            else:
                self.in_ep = endpoint
        try:
            self._drain()
            self._connect()
        except Exception:
            self.__exit__(None, None, None)
            raise
        return self

    def __exit__(self, *exc):
        if self.device is not None:
            try:
                self._command(bytes([DAP_DISCONNECT]))
            except Exception:
                pass
            usb.util.release_interface(self.device, self.interface.bInterfaceNumber)
            usb.util.dispose_resources(self.device)
            self.device = None
        return False

    # A reply left behind by an interrupted client would answer our first
    # command; read until the endpoint is quiet.
    def _drain(self):
        while True:
            try:
                self.in_ep.read(PACKET, DRAIN_TIMEOUT_MS)
            except usb.core.USBTimeoutError:
                return

    def _command(self, packet):
        self.out_ep.write(packet, self.timeout_ms)
        reply = b""
        # The harness may end a previous transfer with a zero-length packet.
        for _ in range(3):
            reply = bytes(self.in_ep.read(PACKET, self.timeout_ms))
            if reply:
                break
        if not reply or reply[0] != packet[0]:
            raise DapError(f"DAP command {packet[0]:#04x} answered {reply[:1].hex()}")
        return reply

    def _sequence(self, bits, data):
        reply = self._command(bytes([DAP_SWJ_SEQUENCE, bits & 0xFF]) + data)
        if reply[1] != DAP_OK:
            raise DapError("SWJ sequence failed")

    def _connect(self):
        if self._command(bytes([DAP_CONNECT, PORT_SWD]))[1] != PORT_SWD:
            raise DapError("the probe refused SWD")
        self._command(bytes([DAP_SWJ_CLOCK]) + struct.pack("<I", self.clock_hz))
        self._command(bytes([DAP_TRANSFER_CONFIGURE, 0, 64, 0, 0, 0]))
        self._command(bytes([DAP_SWD_CONFIGURE, 0]))
        if self.port.targetsel is None:
            self._sequence(*LINE_RESET)
            self._sequence(*JTAG_TO_SWD)
            self._sequence(*LINE_RESET)
            self._sequence(*IDLE)
            try:
                self._dp_read(DP_DPIDR)
            except DapError:
                self._wake()
                self._dp_read(DP_DPIDR)
        else:
            # A multidrop DP answers nothing until it is selected.
            self._wake()
            self._dp_read(DP_DPIDR)
        self._dp_write(DP_ABORT, ABORT_CLEAR_ALL)
        if self.port.targetsel is None:
            self._dp_write(DP_SELECT, DP_SELECT1_BANK)
            self._dp_write(DP_CTRL_STAT, 0)
        self._dp_write(DP_SELECT, 0)
        self.select = 0
        status = self._dp_read(DP_CTRL_STAT)
        if status & POWER_UP_ACK != POWER_UP_ACK:
            self._dp_write(DP_CTRL_STAT, POWER_UP_REQUEST)
            deadline = time.monotonic() + 0.1
            while self._dp_read(DP_CTRL_STAT) & POWER_UP_ACK != POWER_UP_ACK:
                if time.monotonic() > deadline:
                    raise DapError("the debug port did not power up")
        csw = self._ap_read(AP_CSW)
        csw = (csw & ~(CSW_SIZE_MASK | CSW_ADDRINC_MASK)) | CSW_SIZE_32 | CSW_ADDRINC_SINGLE
        self._ap_write(AP_CSW, csw)

    def _wake(self):
        self._sequence(*LINE_RESET)
        self._sequence(*SWD_TO_DORMANT)
        self._sequence(*WAKE_CYCLES)
        self._sequence(*SELECTION_ALERT)
        self._sequence(*ACTIVATE_SWD)
        self._sequence(*LINE_RESET)
        self._sequence(*IDLE)
        if self.port.targetsel is not None:
            # The first packet after the line reset and its idle cycles.
            self._targetsel(self.port.targetsel)
            self._sequence(*IDLE)

    def _targetsel(self, value):
        parity = bin(value).count("1") & 1
        packet = bytes([DAP_SWD_SEQUENCE, 3, 8, TARGETSEL_REQUEST, TARGETSEL_ACK_CYCLES, 33])
        reply = self._command(packet + struct.pack("<I", value) + bytes([parity]))
        if reply[1] != DAP_OK:
            raise DapError("TARGETSEL sequence failed")

    def _transfer(self, requests):
        packet = bytearray([DAP_TRANSFER, 0, len(requests)])
        reads = 0
        for request, value in requests:
            packet.append(request)
            if request & REQUEST_READ:
                reads += 1
            else:
                packet += struct.pack("<I", value)
        reply = self._command(bytes(packet))
        if reply[1] != len(requests) or reply[2] != ACK_OK:
            raise DapError(f"DAP transfer stopped after {reply[1]} of {len(requests)}, ack {reply[2]}")
        return list(struct.unpack_from(f"<{reads}I", reply, 3))

    def _dp_read(self, register):
        return self._transfer([(REQUEST_READ | register, 0)])[0]

    def _dp_write(self, register, value):
        self._transfer([(register, value)])

    def _select_ap(self, register):
        select = self.port.ap_select | (register & 0xF0)
        if select != self.select:
            self._dp_write(DP_SELECT, select)
            self.select = select

    def _ap_read(self, register):
        self._select_ap(register)
        return self._transfer([(REQUEST_AP | REQUEST_READ | (register & 0xC), 0)])[0]

    def _ap_write(self, register, value):
        self._select_ap(register)
        self._transfer([(REQUEST_AP | (register & 0xC), value)])

    def _block(self, register, count, data=None):
        request = REQUEST_AP | (register & 0xC) | (0 if data else REQUEST_READ)
        packet = bytearray([DAP_TRANSFER_BLOCK, 0]) + struct.pack("<H", count) + bytes([request])
        if data:
            packet += struct.pack(f"<{count}I", *data)
        reply = self._command(bytes(packet))
        done = struct.unpack_from("<H", reply, 1)[0]
        if done != count or reply[3] != ACK_OK:
            raise DapError(f"DAP block stopped after {done} of {count}, ack {reply[3]}")
        return [] if data else list(struct.unpack_from(f"<{count}I", reply, 4))

    def _chunks(self, address, count, per_packet):
        while count:
            room = (TAR_WRAP - (address % TAR_WRAP)) // 4
            n = min(count, per_packet, room)
            yield address, n
            address += n * 4
            count -= n

    def read_words(self, address, count):
        words = []
        for start, n in self._chunks(address, count, (PACKET - 4) // 4):
            self._ap_write(AP_TAR, start)
            self._select_ap(AP_DRW)
            words += self._block(AP_DRW, n)
        return words

    def write_words(self, address, values):
        at = 0
        for start, n in self._chunks(address, len(values), (PACKET - 5) // 4):
            self._ap_write(AP_TAR, start)
            self._select_ap(AP_DRW)
            self._block(AP_DRW, n, values[at:at + n])
            at += n
        self._dp_read(DP_RDBUFF)
