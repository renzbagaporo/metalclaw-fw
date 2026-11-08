#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Which DUT images a change needs run, from the files it touches.
#
#   test/rig/select.py --base <commit> [--head HEAD] [--only smoke,bmp388,...]
#
# Prints a JSON list of image names (`smoke`, `bmp388`, `bmp388/spi`, ...).
# A device's own files select its images. A harness engine selects every
# device that declares a binding of the kind it serves, read from the device
# sources, so a new device needs no table here. Documentation selects
# nothing; anything else may reach every device and selects every image.
# Without --base, every image is selected.

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

ENGINES = {
    "SPI_TARGET": ("core/ports/rp2/hal/spi_target.c", "core/include/metalclaw/hal/spi_target.h"),
    "I2C_TARGET": ("core/ports/rp2/hal/i2c_target.c", "core/include/metalclaw/hal/i2c_target.h"),
    "UART": ("core/ports/rp2/hal/uart.c", "core/ports/rp2/hal/uart_pio.c", "core/ports/rp2/hal/uart_internal.h",
             "core/include/metalclaw/hal/uart.h"),
    "GPIO": ("core/ports/rp2/hal/gpio.c", "core/include/metalclaw/hal/gpio.h"),
}
# Every image's run resets and reads the DUT through the probe, so the probe
# is in every image's path.
EVERY_RUN = {"swd_probe"}
NOTHING = re.compile(r"^(docs/.*|.*\.md|LICENSE-.*|\.gitignore)$")
BINDING = re.compile(r"MCLW_BINDING_([A-Z0-9_]+)")


def device_images(device: Path) -> list[str]:
    dut = device / "test" / "dut"
    images = []
    if (dut / "rp2.overlay").is_file():
        images.append(device.name)
    images += [f"{device.name}/{v.name}" for v in sorted(dut.glob("*")) if (v / "rp2.overlay").is_file()]
    return images


def device_kinds(device: Path) -> set[str]:
    kinds = set()
    for source in list(device.glob("*.c")) + list(device.glob("*.h")):
        for kind in BINDING.findall(source.read_text()):
            kinds.add("GPIO" if kind.startswith("GPIO_") else kind)
    return kinds


def all_images(devices: dict) -> list[str]:
    return ["smoke"] + sorted(i for d in devices.values() for i in d["images"])


def select(changed: list[str], devices: dict) -> list[str]:
    chosen = set()
    engine_files = {path: kind for kind, paths in ENGINES.items() for path in paths}
    for path in changed:
        if NOTHING.match(path):
            continue
        parts = path.split("/")
        if parts[0] == "devices" and len(parts) > 2 and parts[1] in devices:
            if parts[1] in EVERY_RUN:
                if parts[2] == "test":
                    chosen.add("smoke")
                    continue
                return all_images(devices)
            chosen.update(devices[parts[1]]["images"])
            continue
        if path in engine_files:
            kind = engine_files[path]
            chosen.update(i for d in devices.values() if kind in d["kinds"] for i in d["images"])
            continue
        return all_images(devices)
    # The smoke image carries the cases every image shares; a device-only
    # change does not need it.
    return sorted(chosen)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--only", default="", help="comma-separated images that may run at all")
    args = parser.parse_args()

    devices = {d.name: {"images": device_images(d), "kinds": device_kinds(d)}
               for d in sorted((ROOT / "devices").iterdir()) if d.is_dir()}
    if args.base:
        out = subprocess.run(["git", "-C", str(ROOT), "diff", "--name-only", f"{args.base}...{args.head}"],
                             check=True, capture_output=True, text=True).stdout
        images = select(out.split(), devices)
    else:
        images = all_images(devices)
    if args.only:
        allowed = set(args.only.split(","))
        images = [i for i in images if i in allowed]
    json.dump(images, sys.stdout)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
