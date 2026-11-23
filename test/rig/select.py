#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Which rig units a change needs run on a platform, from the files it touches.
#
#   test/rig/select.py --platform sim [--base <commit>] [--head HEAD]
#
# Prints a JSON list of units: the smoke image, and one per device package
# with something to run there, its DUT images (devices/<pkg>/test/dut) and
# the upstream Zephyr samples whose main part is one of its models:
#   {"name": "bmp388", "images": ["bmp388", "bmp388/spi"], "samples": ["zephyr/sensor/pressure_polling"]}
# Without --base, every unit. With --shards N, the units packed into at most
# N jobs for a CI matrix, balanced by how many runs each holds:
#   {"name": "bmp388, cellular", "units": [...]}
#
# What a change selects:
#   docs, Markdown, licences                     nothing
#   test/unit, test/host, test/engine            nothing (CI runs them always)
#   devices/<pkg>                                the units running <pkg>'s images or a
#                                                sample that attaches one of its models
#   console_bridge                               also every unit with samples, whose
#                                                consoles it carries
#   a device in the platform's every_run         every unit
#   core/src/<lib>.c, metalclaw/<lib>.h          the devices that include metalclaw/<lib>.h
#     for the device libraries (LIBRARIES)
#   an engine (ENGINES): metalclaw/hal/<x>.h     the devices that declare a binding of
#     or core/ports/<port>/[hal/]<x>.c           its kind; another port's engine nothing
#   test/zephyr_samples/overlays/<file>          the samples built with it
#   anything else (the runtime, the rig, the     every unit
#     DUT app, the Renode pin, the workflows)
# platforms.toml says each platform's port and every_run.

import argparse
import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

# Engine sources by file stem, under metalclaw/hal/ and each port, and the
# binding kind each serves (MCLW_BINDING_<kind>; the GPIO_* kinds as GPIO).
ENGINES = {
    "spi_target": "SPI_TARGET", "i2c_target": "I2C_TARGET", "spi_sink": "SPI_SINK",
    "uart": "UART", "uart_pio": "UART", "uart_internal": "UART",
    "gpio": "GPIO", "gpio_internal": "GPIO", "gpio_link": "GPIO", "spi_peer": "GPIO",
    "wire": "WIRE", "wire_programs": "WIRE",
}
# Core libraries a device uses by including metalclaw/<name>.h.
LIBRARIES = ("regmap", "stream", "sink", "onewire", "command")
NOTHING = re.compile(r"^(docs/.*|.*\.md|LICENSE-.*|\.gitignore|test/(unit|host|engine)/.*)$")
# The consoles of the samples go through it.
CONSOLE = "console_bridge"
# Parts that serve a sample's main part rather than being it: a sample's unit
# is that of its first other part.
HELPERS = {"jumper", "jumper_open_drain", "jumper_open_drain_clock", "jumper_open_drain_clock_edge",
           "square_wave"}
BINDING = re.compile(r"MCLW_BINDING_([A-Z0-9_]+)")
MODEL_ID = re.compile(r'\.id = "([a-z0-9_]+)"|mclw_model_\w+\s*,\s*"([a-z0-9_]+)"')
INCLUDE = re.compile(r'#include "metalclaw/(\w+)\.h"')


def _sources(device: Path) -> list[Path]:
    return sorted(device.glob("*.c")) + sorted(device.glob("*.h"))


def device_images(device: Path) -> list[str]:
    dut = device / "test" / "dut"
    images = []
    if (dut / "rp2.overlay").is_file():
        images.append(device.name)
    images += [f"{device.name}/{v.name}" for v in sorted(dut.glob("*")) if (v / "rp2.overlay").is_file()]
    return images


def scan_devices() -> dict:
    devices = {}
    for d in sorted((ROOT / "devices").iterdir()):
        if not d.is_dir():
            continue
        text = "".join(p.read_text() for p in _sources(d))
        kinds = {"GPIO" if k.startswith("GPIO_") else k for k in BINDING.findall(text)}
        devices[d.name] = {
            "images": device_images(d),
            "kinds": kinds,
            "libraries": set(INCLUDE.findall(text)) & set(LIBRARIES),
            "models": {a or b for a, b in MODEL_ID.findall(text)},
        }
    return devices


def scan_samples(devices: dict) -> dict:
    # Each sample: the packages of the models it attaches, its main one, and
    # the overlays and confs it is built with.
    from test.zephyr_samples.builds import BUILDS
    from test.zephyr_samples.parts import ZEPHYR_PARTS
    package = {m: name for name, d in devices.items() for m in d["models"]}
    samples = {}
    for target, build in BUILDS.items():
        models = [a.model for a in ZEPHYR_PARTS.get(target, ())]
        if not models:
            continue
        main = next((m for m in models if m not in HELPERS), models[0])
        samples[target] = {
            "packages": {package[m] for m in models} | {CONSOLE},
            "unit": package[main],
            "files": {Path(f).name for f in (*build.overlays, *build.confs)},
        }
    return samples


def units(devices: dict, samples: dict) -> dict:
    found = {"smoke": {"name": "smoke", "images": ["smoke"], "samples": []}}
    for name, d in devices.items():
        if d["images"]:
            found[name] = {"name": name, "images": list(d["images"]), "samples": []}
    for target, s in sorted(samples.items()):
        found.setdefault(s["unit"], {"name": s["unit"], "images": [], "samples": []})["samples"].append(target)
    return found


def engine(path: str, port: str) -> tuple[str, bool] | None:
    # The binding kind an engine file serves, and whether the platform runs
    # it; None for a file that is no engine.
    p = Path(path)
    if p.suffix not in (".c", ".h") or p.stem not in ENGINES:
        return None
    parts = p.parts
    if parts[:3] == ("core", "include", "metalclaw") and parts[3:4] == ("hal",):
        return ENGINES[p.stem], True
    if parts[:2] == ("core", "ports") and len(parts) >= 4:
        return ENGINES[p.stem], parts[2] == port
    return None


def library(path: str) -> str | None:
    p = Path(path)
    if (p.parent == Path("core/src") and p.suffix == ".c") or (p.parent == Path("core/include/metalclaw")
                                                                and p.suffix == ".h"):
        return p.stem if p.stem in LIBRARIES else None
    return None


def select(changed: list[str], platform: dict, devices: dict, samples: dict) -> list[dict]:
    every = units(devices, samples)
    packages, sample_files = set(), set()
    for path in changed:
        if NOTHING.match(path):
            continue
        parts = path.split("/")
        if parts[0] == "devices" and len(parts) > 2 and parts[1] in devices:
            if parts[1] in platform.get("every_run", ()):
                return list(every.values())
            packages.add(parts[1])
            continue
        if path.startswith("test/zephyr_samples/overlays/"):
            sample_files.add(Path(path).name)
            continue
        lib = library(path)
        if lib is not None:
            packages |= {n for n, d in devices.items() if lib in d["libraries"]}
            continue
        served = engine(path, platform["port"])
        if served is not None:
            kind, runs_here = served
            if runs_here:
                packages |= {n for n, d in devices.items() if kind in d["kinds"]}
            continue
        return list(every.values())
    chosen = {n for n in packages if n in every and devices[n]["images"]}
    for target, s in samples.items():
        if s["packages"] & packages or s["files"] & sample_files:
            chosen.add(s["unit"])
    # A unit runs only what the change reaches: a device's own images when
    # it changed, the samples the change reaches.
    picked = []
    for name in sorted(chosen):
        unit = every[name]
        images = unit["images"] if name in packages else []
        reached = [t for t in unit["samples"] if samples[t]["packages"] & packages or samples[t]["files"] & sample_files]
        if images or reached:
            picked.append({"name": name, "images": images, "samples": reached})
    return picked


def shards(chosen: list[dict], count: int, boards: int) -> list[dict]:
    # Greedy, largest unit first, into the job with the fewest runs; the
    # samples count twice, each a build and a run.
    jobs = [[] for _ in range(min(count, len(chosen)))]
    load = [0] * len(jobs)

    def cost(unit: dict) -> int:
        return boards * (len(unit["images"]) + 2 * len(unit["samples"]))
    for unit in sorted(chosen, key=cost, reverse=True):
        i = load.index(min(load))
        jobs[i].append(unit)
        load[i] += cost(unit)
    return [{"name": ", ".join(u["name"] for u in sorted(job, key=lambda u: u["name"])), "units": job}
            for job in jobs if job]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", required=True)
    parser.add_argument("--base")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--shards", type=int)
    args = parser.parse_args()

    platforms = tomllib.loads((ROOT / "test/rig/platforms.toml").read_text())["platforms"]
    if args.platform not in platforms:
        parser.error(f"no platform {args.platform!r} in test/rig/platforms.toml")
    devices = scan_devices()
    samples = scan_samples(devices)
    if args.base:
        out = subprocess.run(["git", "-C", str(ROOT), "diff", "--name-only", f"{args.base}...{args.head}"],
                             check=True, capture_output=True, text=True).stdout
        chosen = select(out.split(), platforms[args.platform], devices, samples)
    else:
        chosen = list(units(devices, samples).values())
    if args.shards:
        chosen = shards(chosen, args.shards, len(platforms[args.platform]["boards"]))
    json.dump(chosen, sys.stdout)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
