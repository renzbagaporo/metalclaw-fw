# SPDX-License-Identifier: Apache-2.0 OR MIT
# The USB device a pico-examples program presents on the DUT's own USB, from
# its descriptors: how the rig finds it once the DUT's USB is imported into
# the rig host (the emulator platform's `dut_usb`), and what it must show
# there. Neither bench cables the DUT's USB to the rig host.

from dataclasses import dataclass


@dataclass(frozen=True)
class UsbDevice:
    vid: int
    pid: int
    # The interface strings of its CDC ACM functions, in interface order.
    ttys: tuple[str, ...] = ()
    # How many HID interfaces it has, each a hidraw node.
    hidraw: int = 0
    # Its endpoints but EP0, by bEndpointAddress.
    endpoints: tuple[int, ...] = ()
    source: str = ""


# pico-sdk 2.3.0 src/rp2_common/pico_stdio_usb/stdio_usb_descriptors.c:36-45,
# 136: Raspberry Pi's VID, PID 000a on RP2040 and 0009 otherwise, one CDC
# function named "Board CDC" (and a vendor reset interface, no tty).
STDIO_USB_PID = {"rp2040": 0x000A, "rp2350": 0x0009}


def _stdio_usb(source: str) -> dict[str, UsbDevice]:
    return {chip: UsbDevice(0x2E8A, pid, ("Board CDC",), source=source) for chip, pid in STDIO_USB_PID.items()}


def _both(device: UsbDevice) -> dict[str, UsbDevice]:
    return {chip: device for chip in STDIO_USB_PID}


# By target, then by chip (example.json "chip").
USB_DEVICES: dict[str, dict[str, UsbDevice]] = {
    "hello_usb": _stdio_usb("hello_world/usb/CMakeLists.txt"),
    "hello_anything": _stdio_usb("binary_info/hello_anything/CMakeLists.txt"),
    # usb/device/dev_hid_composite/usb_descriptors.c:36-40, 101-115 and
    # tusb_config.h:94-98: VID cafe, PID 4000 | HID << 2, one HID interface.
    "dev_hid_composite": _both(UsbDevice(0xCAFE, 0x4004, hidraw=1,
                                         source="usb/device/dev_hid_composite/usb_descriptors.c")),
    # usb/device/dev_multi_cdc/usb_descriptors.c:15-18, 38-43 and
    # tusb_config.h:28: VID cafe, PID 4100 | CDC count, two CDC functions.
    "dev_multi_cdc": _both(UsbDevice(0xCAFE, 0x4102, ("Pico SDK stdio", "Custom CDC"),
                                     source="usb/device/dev_multi_cdc/usb_descriptors.c")),
    # usb/device/dev_lowlevel/dev_lowlevel.h:73-74, 97-110: VID 0000, PID
    # 0001, a vendor interface with bulk EP1 OUT and EP2 IN.
    "dev_lowlevel": _both(UsbDevice(0x0000, 0x0001, endpoints=(0x01, 0x82),
                                    source="usb/device/dev_lowlevel/dev_lowlevel.h")),
}


def usb_device(target: str, chip: str | None) -> UsbDevice | None:
    return USB_DEVICES.get(target, {}).get(chip)
