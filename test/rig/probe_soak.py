#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR MIT
# Soaks one bench's SWD path and logs it in the probe telemetry format
# (test/rig/probe_telemetry.py), so test/rig/probe_report.py rolls soaks and
# rig sessions up the same way.
#
#   flash   N cycles of `probe-rs download --verify`, then `probe-rs reset`,
#           through the harness probe, alternating two DUT images so the
#           flash changes every cycle. Leaves the first image flashed.
#   memory  for a while, with the DUT running an image: write a pseudo-random
#           block to the image's scratch RAM, read it back and compare, and
#           read a window of the image's flash and compare it with the ELF.
#           Through the harness probe by the rig's own DAP client, or through
#           a J-Link (--jlink) for a baseline.
#
#   test/rig/probe_soak.py flash --serial <serial> \
#       --image build/dut/smoke/zephyr/zephyr.elf \
#       --image build/dut/bmp388/zephyr/zephyr.elf --log soak.jsonl
#   test/rig/probe_soak.py memory --serial <serial> \
#       --image build/dut/smoke/zephyr/zephyr.elf --duration 1h --log soak.jsonl
#   test/rig/probe_soak.py memory --jlink <J-Link serial> \
#       --image build/dut/smoke/zephyr/zephyr.elf --duration 1h --log soak.jsonl
#
# Each run appends one line per flash cycle and per failed memory access,
# and one session line at the end, to --log (default $METALCLAW_PROBE_LOG),
# and prints the session's rollup. Ctrl-C or SIGTERM ends a soak early; the
# session line and the summary still come out, marked interrupted.

import argparse
import importlib
import importlib.util
import json
import os
import random
import signal
import subprocess
import sys
import time
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from test.rig import probe_report, probe_telemetry  # noqa: E402
from test.rig import dut_flash  # noqa: E402
from test.rig.platforms.hardware import dap  # noqa: E402
from test.rig.platforms.hardware import mailbox  # noqa: E402
from test.rig.platforms.hardware.platform import (  # noqa: E402
    DHCSR,
    DHCSR_RESUME,
    DUT_CHIPS,
    Harness,
    board_fixture,
    soc_series,
)
from test.rig.platforms.hardware.probe import ProbeError, ProbeRs  # noqa: E402

SCRATCH_SYMBOL = "mclw_dut_scratch"
# Each DUT chip's SRAM, the RP2350's scratch X and Y banks included.
SRAM = {"rp2350": (0x20000000, 0x20082000), "rp2040": (0x20000000, 0x20042000)}
JLINK_DEVICES = {"rp2350": "RP2350_M33_0", "rp2040": "RP2040_M0_0"}
JLINK_USB = "1366:0101"
HARNESS_USB = f"{dap.VENDOR_ID:04x}:{dap.PRODUCT_ID:04x}"
SOAK = "soak"
EVENTS = 1000
PROGRESS_S = 60.0
ERROR_BACKOFF_S = 0.2
DHCSR_S_HALT = 1 << 17
MAILBOX_HEADER = (mailbox.MAGIC0, mailbox.RESULT_MAGIC1, mailbox.VERSION, mailbox.RESULT_CAPACITY)
# Data the image places in RAM, by nm's type letter.
RAM_OBJECT_TYPES = set("bBdD")


class SoakError(RuntimeError):
    pass


def parse_duration(text: str) -> float:
    units = {"s": 1.0, "m": 60.0, "h": 3600.0}
    text = text.strip().lower()
    scale = units.get(text[-1:], None)
    number = text[:-1] if scale else text
    try:
        seconds = float(number) * (scale or 1.0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid duration {text!r}; e.g. 90, 90s, 30m, 1h") from error
    if not seconds > 0:
        raise argparse.ArgumentTypeError(f"invalid duration {text!r}; it must be positive")
    return seconds


def parse_scratch(text: str) -> tuple[int, int]:
    try:
        address, size = (int(part, 0) for part in text.split(":"))
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid scratch area {text!r}; expected ADDRESS:BYTES") from error
    return address, size


def pattern(seed: int, iteration: int, count: int) -> list[int]:
    generator = random.Random(seed * 1_000_003 + iteration)
    return [generator.getrandbits(32) for _ in range(count)]


# Every word that differs, as (address, expected, read); a read that came
# back short reports the missing words as None.
def compare(address: int, expected: list[int], got: list[int]) -> list[tuple[int, int, int | None]]:
    differences = []
    for index, word in enumerate(expected):
        read = got[index] if index < len(got) else None
        if read != word:
            differences.append((address + 4 * index, word, read))
    return differences


def describe_differences(differences, shown: int = 4) -> str:
    text = ", ".join(f"{address:#010x}: wrote {expected:#010x} read "
                     f"{'nothing' if got is None else f'{got:#010x}'}"
                     for address, expected, got in differences[:shown])
    more = len(differences) - shown
    return f"{len(differences)} words differ: {text}" + (f", and {more} more" if more > 0 else "")


# The scratch area in blocks of at most `words`, round and round.
def blocks(base: int, size: int, words: int):
    total = size // 4
    if total <= 0 or words <= 0:
        raise SoakError("the scratch area and the block must each hold at least one word")
    while True:
        for offset in range(0, total, words):
            yield base + 4 * offset, min(words, total - offset)


def words_of(data: bytes) -> list[int]:
    return [int.from_bytes(data[index:index + 4], "little") for index in range(0, len(data) - len(data) % 4, 4)]


# Windows of at most `words` over every whole word the image puts in flash,
# with the words the ELF says they hold, round and round.
def flash_windows(segments: list[tuple[int, bytes]], words: int):
    windows = []
    for address, data in segments:
        skip = -address % 4
        start = address + skip
        values = words_of(data[skip:])
        for offset in range(0, len(values), words):
            windows.append((start + 4 * offset, values[offset:offset + words]))
    if not windows:
        raise SoakError("the image puts no whole word in flash")
    while True:
        yield from windows


# `nm -S` lines: address, optional size, type, name.
def parse_nm(text: str) -> dict[str, tuple[int, int | None, str]]:
    symbols = {}
    for line in text.splitlines():
        fields = line.split()
        try:
            if len(fields) == 4:
                symbols[fields[3]] = (int(fields[0], 16), int(fields[1], 16), fields[2])
            elif len(fields) == 3:
                symbols[fields[2]] = (int(fields[0], 16), None, fields[1])
        except ValueError:
            continue
    return symbols


def image_symbols(image: Path, toolchain_dir: str | None) -> dict[str, tuple[int, int | None, str]]:
    nm = mailbox.toolchain_nm(toolchain_dir)
    try:
        completed = subprocess.run([str(nm), "-S", str(image)], capture_output=True, text=True,
                                   timeout=mailbox.NM_TIMEOUT_S, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SoakError(f"{nm} failed on {image}: {error}") from error
    if completed.returncode != 0:
        raise SoakError(f"{nm} exited {completed.returncode}: {completed.stderr.strip()}")
    return parse_nm(completed.stdout)


def _overlaps(start: int, end: int, other_start: int, other_end: int) -> bool:
    return start < other_end and other_start < end


# Where the memory soak may write: the image's scratch symbol, or an area
# given outright that holds none of the image's RAM objects, and no heap the
# image may hand out.
def scratch_area(given, symbols: dict, series: str, config: str = "") -> tuple[int, int]:
    low, high = SRAM[series]
    if given is None:
        found = symbols.get(SCRATCH_SYMBOL)
        if found is None or not found[1]:
            raise SoakError(f"the image has no {SCRATCH_SYMBOL} (rebuild it from test/rig/dut), "
                            "and no --scratch was given; the soak writes nowhere else")
        address, size = found[0], found[1] - found[1] % 4
    else:
        address, size = given
    end = address + size
    if address % 4 or size < 4 or size % 4:
        raise SoakError(f"the scratch area {address:#x}:{size} is not whole aligned words")
    if not (low <= address and end <= high):
        raise SoakError(f"the scratch area {address:#x}..{end:#x} is not in the DUT's SRAM {low:#x}..{high:#x}")
    if given is None:
        return address, size
    for name, (start, length, kind) in symbols.items():
        if name != SCRATCH_SYMBOL and kind in RAM_OBJECT_TYPES and length and _overlaps(address, end, start, start + length):
            raise SoakError(f"the scratch area {address:#x}..{end:#x} overlaps the image's {name}")
    if "CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=-1" in config.splitlines() and "_end" in symbols:
        if _overlaps(address, end, symbols["_end"][0], high):
            raise SoakError(f"the scratch area {address:#x}..{end:#x} is in the image's malloc arena, "
                            f"all of RAM from _end ({symbols['_end'][0]:#x})")
    if "__GcHeapStart" in symbols and "__GcHeapEnd" in symbols:
        if _overlaps(address, end, symbols["__GcHeapStart"][0], symbols["__GcHeapEnd"][0]):
            raise SoakError(f"the scratch area {address:#x}..{end:#x} is in the image's MicroPython heap")
    return address, size


class Log:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.file = self.path.open("a")
        self.events = 0
        self.suppressed = 0

    def write(self, record: dict) -> None:
        self.file.write(json.dumps(record, sort_keys=True) + "\n")
        self.file.flush()

    # Failures, as they happen, up to EVENTS of them; the rest are counted.
    def event(self, record: dict) -> None:
        if self.events >= EVENTS:
            self.suppressed += 1
            return
        self.events += 1
        self.write({"record": "event", **record})

    def close(self) -> None:
        self.file.close()


def access_kind(error: BaseException) -> str:
    return dap.error_kind(error)


# One step of a flash cycle with up to `retries` more tries, as the cycle's
# line records it.
def attempt(call, retries: int, recorder, layer: str, operation: str, clock=time.monotonic) -> dict:
    started = clock()
    errors = []
    for number in range(retries + 1):
        try:
            call()
        except Exception as error:
            errors.append({"kind": access_kind(error), "message": str(error)[:probe_telemetry.MESSAGE_CHARS]})
            if number:
                recorder.retry(layer, operation, False)
            continue
        if number:
            recorder.retry(layer, operation, True)
        return {"ok": True, "attempts": number + 1, "seconds": round(clock() - started, 3), "errors": errors}
    return {"ok": False, "attempts": retries + 1, "seconds": round(clock() - started, 3), "errors": errors}


class Progress:
    def __init__(self, every: float = PROGRESS_S, clock=time.monotonic, out=sys.stderr):
        self.clock = clock
        self.every = every
        self.out = out
        self.started = clock()
        self.next = self.started + every

    def __call__(self, text_of) -> None:
        now = self.clock()
        if now >= self.next:
            self.next = now + self.every
            print(f"probe_soak: {now - self.started:.0f} s: {text_of()}", file=self.out, flush=True)


def flash_soak(probe, images: list[Path], cycles: int, retries: int, max_failed: int, recorder, log: Log,
               state: dict, progress=lambda text_of: None, clock=time.monotonic) -> None:
    consecutive = 0
    for cycle in range(cycles):
        image = images[cycle % len(images)]
        started = clock()
        download = attempt(lambda: probe.download(image, verify=True, reset=False), retries, recorder,
                           ProbeRs.LAYER, "download", clock)
        state["flashed"] = image if download["ok"] else None
        reset = attempt(probe.reset, retries, recorder, ProbeRs.LAYER, "reset", clock) if download["ok"] else None
        ok = download["ok"] and reset["ok"]
        failure = None if ok else (download if not download["ok"] else reset)["errors"][-1]
        recorder.record(SOAK, "flash_cycle", clock() - started, "ok" if ok else failure["kind"],
                        None if ok else failure["message"])
        log.write({"record": "cycle", "mode": "flash", "cycle": cycle, "image": str(image), "ok": ok,
                   "download": download, "reset": reset})
        state["cycles"] = cycle + 1
        state["failed_cycles"] += 0 if ok else 1
        consecutive = 0 if ok else consecutive + 1
        if consecutive >= max_failed:
            state["stopped"] = f"{consecutive} cycles in a row failed"
            return
        progress(lambda: f"{state['cycles']} of {cycles} cycles, {state['failed_cycles']} failed")


def memory_soak(memory, recorder, log: Log, state: dict, *, scratch, windows, block_words: int, flash_every: int,
                duration: float, seed: int, max_consecutive: int, clock=time.monotonic, sleep=time.sleep,
                progress=lambda text_of: None) -> None:
    sram = None if scratch is None else blocks(scratch[0], scratch[1], block_words)
    deadline = clock() + duration
    consecutive = 0

    def cycle(operation, address, expected, act):
        started = recorder.clock()
        try:
            got = act()
        except Exception as error:
            kind = access_kind(error)
            recorder.record(SOAK, operation, recorder.clock() - started, kind, error)
            log.event({"at_s": round(clock() - state["started"], 3), "operation": operation, "address": address,
                       "kind": kind, "message": str(error)[:probe_telemetry.MESSAGE_CHARS]})
            return False
        differences = compare(address, expected, got)
        if differences:
            text = describe_differences(differences)
            recorder.record(SOAK, operation, recorder.clock() - started, "mismatch", text)
            log.event({"at_s": round(clock() - state["started"], 3), "operation": operation, "address": address,
                       "kind": "mismatch", "message": text})
            state["mismatched_words"] += len(differences)
        else:
            recorder.record(SOAK, operation, recorder.clock() - started)
        return True

    while clock() < deadline:
        iteration = state["iterations"]
        reached = True
        if sram is not None:
            address, count = next(sram)
            expected = pattern(seed, iteration, count)

            def write_then_read():
                memory.write_words(address, expected)
                return memory.read_words(address, count)

            reached = cycle("sram_cycle", address, expected, write_then_read)
        if windows is not None and iteration % flash_every == 0:
            address, expected = next(windows)
            reached = cycle("flash_cycle", address, expected,
                            lambda: memory.read_words(address, len(expected))) and reached
        state["iterations"] = iteration + 1
        consecutive = 0 if reached else consecutive + 1
        if consecutive >= max_consecutive:
            state["stopped"] = f"{consecutive} iterations in a row could not reach the DUT"
            return
        if not reached:
            sleep(ERROR_BACKOFF_S)
        progress(lambda: f"{state['iterations']} iterations, {state['mismatched_words']} words mismatched, "
                         f"{log.events + log.suppressed} failures")


# DUT memory through a J-Link by pylink, over one connection that is made
# again after a failure.
class JLinkMemory:
    LAYER = "jlink"

    def __init__(self, serial: str, device: str, speed_khz: int, recorder, pylink=None):
        self.pylink = pylink or importlib.import_module("pylink")
        self.serial = int(serial)
        self.device = device
        self.speed_khz = speed_khz
        self.recorder = recorder
        self.link = None

    def _connect(self) -> None:
        recorder = self.recorder
        started = recorder.clock()
        link = self.pylink.JLink()
        try:
            link.open(serial_no=self.serial)
            link.set_tif(self.pylink.enums.JLinkInterfaces.SWD)
            link.connect(self.device, speed=self.speed_khz)
        except Exception as error:
            recorder.record(self.LAYER, "connect", recorder.clock() - started, access_kind(error), error)
            self._close(link)
            raise
        recorder.record(self.LAYER, "connect", recorder.clock() - started)
        self.link = link

    @staticmethod
    def _close(link) -> None:
        try:
            link.close()
        except Exception:
            pass

    def _access(self, operation: str, words: int, act):
        recorder = self.recorder
        started = recorder.clock()
        try:
            if self.link is None:
                self._connect()
            result = act(self.link)
        except Exception as error:
            recorder.record(self.LAYER, operation, recorder.clock() - started, access_kind(error), error)
            if self.link is not None:
                self._close(self.link)
                self.link = None
            raise
        recorder.record(self.LAYER, operation, recorder.clock() - started, words=words)
        return result

    def read_words(self, address: int, count: int) -> list[int]:
        return list(self._access("read", count, lambda link: link.memory_read32(address, count)))

    def write_words(self, address: int, words) -> None:
        values = [value & 0xFFFFFFFF for value in words]
        self._access("write", len(values), lambda link: link.memory_write32(address, values))

    def close(self) -> None:
        if self.link is not None:
            self._close(self.link)
            self.link = None


def image_chip(image: Path, series=None):
    if not image.is_file():
        raise SoakError(f"no image at {image}")
    found = soc_series(image) or series
    if found not in DUT_CHIPS:
        raise SoakError(f"{image.parent / '.config'} names no DUT chip the rig knows "
                        f"(CONFIG_SOC_SERIES={found!r}); pass --chip-series for an image without one")
    return found, DUT_CHIPS[found]


def _config(image: Path) -> str:
    config = image.parent / ".config"
    return config.read_text() if config.is_file() else ""


class Bench:
    """The harness, when the soak names one: checked present with its probe,
    and asked for the probe's counters as the soak starts and ends."""

    def __init__(self, serial: str | None, attach: bool):
        self.serial = serial
        self.harness = None
        if serial is None:
            return
        if dap.find_harness(serial) is None:
            raise SoakError(f"no harness {serial} on USB")
        self.harness = Harness(serial)
        if dap.INTERFACE_NAME not in self.harness.usb_interfaces():
            if not attach:
                raise SoakError(f"harness {serial} has no {dap.INTERFACE_NAME} interface: no swd_probe is attached "
                                "(any rig run attaches it; or pass --attach)")
            swd = board_fixture(self.harness)["swd"]
            self.harness.run(f"import metalclaw\nmetalclaw.attach('swd_probe', 'dut', swd={swd})")
            self.harness.wait_interface(dap.INTERFACE_NAME)

    def snapshot(self):
        if self.harness is None:
            return None, "no harness named"
        return probe_telemetry.read_probe(self.harness.run, 5.0)

    def close(self) -> None:
        if self.harness is not None:
            self.harness.close()


def _harness_probe(serial: str, chip, speed_khz: int, recorder) -> ProbeRs:
    memory = dap.DapMemory(serial, chip.debug_port, recorder=recorder)
    # A probe-rs session cut off by its deadline leaves the core halted with
    # debug on, as the rig's Dut.resume undoes.
    return ProbeRs(f"{HARNESS_USB}:{serial}", chip.chip_description, speed_khz, chip=chip.probe_chip,
                   on_failure=lambda: memory.write_words(DHCSR, [DHCSR_RESUME]), recorder=recorder)


def _flash_and_reset(probe: ProbeRs, image: Path, retries: int, recorder) -> None:
    for step, call in (("download", lambda: probe.download(image, verify=True, reset=False)),
                       ("reset", probe.reset)):
        result = attempt(call, retries, recorder, ProbeRs.LAYER, step)
        if not result["ok"]:
            raise SoakError(f"{step} of {image} failed: {result['errors'][-1]['message']}")


def run_flash(args, recorder, log: Log, state: dict, bench: Bench) -> None:
    images = [Path(image) for image in args.image]
    if len(images) != 2:
        raise SoakError("the flash soak takes exactly two --image, so the flash changes every cycle")
    series = [image_chip(image, args.chip_series) for image in images]
    if series[0][0] != series[1][0]:
        raise SoakError(f"the images are for different chips: {series[0][0]} and {series[1][0]}")
    contents = [dut_flash.flash_contents(image.read_bytes()) for image in images]
    if contents[0] == contents[1]:
        raise SoakError("the two images put the same bytes in flash; the soak would not change the flash")
    chip = series[0][1]
    recorder.describe(dut={"chip": chip.probe_chip, "series": series[0][0], "images": [str(i) for i in images]})
    probe = _harness_probe(args.serial, chip, args.speed_khz, recorder)
    state.update(cycles=0, failed_cycles=0, flashed=None)
    restore = None if args.no_restore else images[0]
    try:
        flash_soak(probe, images, args.cycles, args.retries, args.max_failed_cycles, recorder, log, state,
                   Progress())
    finally:
        if restore is not None and state.get("flashed") != restore:
            try:
                _flash_and_reset(probe, restore, args.retries, recorder)
                state["restored"] = str(restore)
            except SoakError as error:
                state["restored"] = f"failed: {error}"
        state.pop("flashed", None)


def run_memory(args, recorder, log: Log, state: dict, bench: Bench) -> None:
    image = Path(args.image[0]) if len(args.image) == 1 else None
    if image is None:
        raise SoakError("the memory soak takes exactly one --image, the one the DUT runs")
    series, chip = image_chip(image, args.chip_series)
    symbols = image_symbols(image, args.toolchain)
    scratch = None if args.read_only else scratch_area(args.scratch, symbols, series, _config(image))
    segments = dut_flash.flash_segments(image.read_bytes())
    recorder.describe(dut={"chip": chip.probe_chip, "series": series, "image": str(image)})
    if args.jlink and args.flash:
        # An external J-Link may be wired to the harness's own SWD pads rather
        # than the DUT's; refuse flashing through it so a DUT image cannot
        # overwrite the harness.
        raise SoakError("the soak never flashes through a J-Link; flash the DUT through the harness first")
    if args.jlink:
        probe_id = f"{JLINK_USB}:{args.jlink}"
        flasher = ProbeRs(probe_id, chip.chip_description, args.speed_khz, chip=chip.probe_chip, recorder=recorder)
        backend = args.backend or ("pylink" if importlib.util.find_spec("pylink") else "probe-rs")
        if backend == "pylink":
            memory = JLinkMemory(args.jlink, args.jlink_device or JLINK_DEVICES[series], args.speed_khz, recorder)
        else:
            memory = flasher
        recorder.describe(soak={**recorder.context.get("soak", {}), "probe_id": probe_id, "backend": backend})
    else:
        flasher = _harness_probe(args.serial, chip, args.speed_khz, recorder)
        memory = dap.DapMemory(args.serial, chip.debug_port, args.clock_hz, recorder)
        recorder.describe(soak={**recorder.context.get("soak", {}), "backend": "dap"})
    if args.flash:
        _flash_and_reset(flasher, image, 2, recorder)
    try:
        check_running(memory, image, symbols, segments, args.flash_words, state)
        state.update(iterations=0, mismatched_words=0, scratch=None if scratch is None else
                     {"address": f"{scratch[0]:#010x}", "bytes": scratch[1]})
        memory_soak(memory, recorder, log, state, scratch=scratch,
                    windows=None if args.flash_every == 0 else flash_windows(segments, args.flash_words),
                    block_words=args.block_words, flash_every=max(1, args.flash_every), duration=args.duration,
                    seed=args.seed, max_consecutive=args.max_consecutive_errors, progress=Progress())
    finally:
        if isinstance(memory, JLinkMemory):
            memory.close()


# The DUT holds the image (its first flash window matches the ELF) and runs
# it (its mailbox header is initialised); a halted core is resumed, as the
# rig does after a cut-off probe-rs session.
def check_running(memory, image: Path, symbols: dict, segments, words: int, state: dict) -> None:
    address, expected = next(flash_windows(segments[:1], words))
    differences = compare(address, expected, memory.read_words(address, len(expected)))
    if differences:
        raise SoakError(f"the DUT's flash does not hold {image} ({describe_differences(differences, 2)}); "
                        "pass --flash to download it first")
    if memory.read_words(DHCSR, 1)[0] & DHCSR_S_HALT:
        memory.write_words(DHCSR, [DHCSR_RESUME])
        state["resumed_halted_core"] = True
    result = symbols.get(mailbox.RESULT_SYMBOL)
    if result is not None:
        header = tuple(memory.read_words(result[0], len(MAILBOX_HEADER)))
        if header != MAILBOX_HEADER:
            raise SoakError(f"the DUT is not running {image}: its mailbox header reads "
                            f"{' '.join(f'{word:#010x}' for word in header)}; reset it, or pass --flash")


def parser() -> argparse.ArgumentParser:
    top = argparse.ArgumentParser(description="Soak one bench's SWD probe and log it as probe telemetry.")
    modes = top.add_subparsers(dest="mode", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--image", action="append", required=True, type=Path,
                        help="a DUT zephyr.elf, with its .config beside it (flash: give two; memory: one)")
    common.add_argument("--log", type=Path, default=os.environ.get(probe_telemetry.LOG_ENV) or None,
                        help=f"JSON-lines file to append to (default ${probe_telemetry.LOG_ENV})")
    common.add_argument("--speed-khz", type=int, default=1000, help="probe-rs SWD speed (default 1000)")
    common.add_argument("--chip-series", choices=sorted(DUT_CHIPS), help="the chip, for an image with no .config")
    common.add_argument("--attach", action="store_true",
                        help="attach swd_probe on the harness board's SWD pins if it is not attached")

    flash = modes.add_parser("flash", parents=[common], help="download and reset, alternating two images")
    flash.add_argument("--serial", required=True, help="the harness's USB serial (METALCLAW_HARNESS_SERIAL)")
    flash.add_argument("--cycles", type=int, default=1000, help="download-and-reset cycles (default 1000)")
    flash.add_argument("--retries", type=int, default=2, help="tries after a failed download or reset (default 2)")
    flash.add_argument("--max-failed-cycles", type=int, default=5,
                       help="stop after this many failed cycles in a row (default 5)")
    flash.add_argument("--no-restore", action="store_true",
                       help="leave whichever image was flashed last, not the first --image")

    memory = modes.add_parser("memory", parents=[common], help="write, read back and compare DUT RAM and flash")
    via = memory.add_mutually_exclusive_group(required=True)
    via.add_argument("--serial", help="the harness's USB serial: soak the harness probe through the rig's DAP client")
    via.add_argument("--jlink", metavar="SERIAL",
                     help=f"a J-Link's serial ({JLINK_USB}:SERIAL): the same soak through it, as a baseline")
    memory.add_argument("--harness", metavar="SERIAL", help="with --jlink: the bench's harness, to log its probe's "
                        "counters and board")
    memory.add_argument("--backend", choices=("pylink", "probe-rs"),
                        help="with --jlink: pylink (one connection) or probe-rs (one session per access, which "
                             "halts the core); default pylink when it is installed")
    memory.add_argument("--jlink-device", help="with --jlink and pylink: the J-Link device name "
                        f"(default {', '.join(f'{k} {v}' for k, v in JLINK_DEVICES.items())})")
    memory.add_argument("--duration", type=parse_duration, default=3600.0, help="how long: 90, 90s, 30m, 1h (default 1h)")
    memory.add_argument("--scratch", type=parse_scratch,
                        help=f"ADDRESS:BYTES of RAM to write; default the image's {SCRATCH_SYMBOL}")
    memory.add_argument("--read-only", action="store_true", help="write nothing: compare flash only")
    memory.add_argument("--block-words", type=int, default=64, help="words per write and read-back (default 64)")
    memory.add_argument("--flash-words", type=int, default=64, help="words per flash window (default 64)")
    memory.add_argument("--flash-every", type=int, default=1,
                        help="compare a flash window every this many iterations; 0 never (default 1)")
    memory.add_argument("--seed", type=int, default=None, help="pattern seed (default from the clock; logged)")
    memory.add_argument("--clock-hz", type=int, default=4_000_000, help="the DAP client's SWD clock (default 4 MHz)")
    memory.add_argument("--max-consecutive-errors", type=int, default=100,
                        help="stop after this many iterations in a row that cannot reach the DUT (default 100)")
    memory.add_argument("--flash", action="store_true", help="download the image and reset the DUT first")
    memory.add_argument("--toolchain", default=os.environ.get(mailbox.TOOLCHAIN_ENV),
                        help=f"directory holding {mailbox.TOOLCHAIN_NAME}, for nm (default ${mailbox.TOOLCHAIN_ENV})")
    return top


def _interrupt(signum, frame):
    raise KeyboardInterrupt


def main(argv=None) -> int:
    args = parser().parse_args(argv)
    if args.log is None:
        print(f"probe_soak: give --log or set {probe_telemetry.LOG_ENV}", file=sys.stderr)
        return 2
    if args.mode == "memory" and args.seed is None:
        args.seed = time.time_ns() % (1 << 32)
    harness_serial = args.serial if args.mode == "flash" or args.serial else getattr(args, "harness", None)
    recorder = probe_telemetry.Recorder(SOAK)
    recorder.describe(platform="hardware", bench={"serial": harness_serial},
                      soak={"mode": args.mode, "seed": getattr(args, "seed", None),
                            "argv": sys.argv[1:] if argv is None else list(argv)})
    state = {"started": time.monotonic(), "stopped": None, "interrupted": False}
    signal.signal(signal.SIGTERM, _interrupt)
    log = Log(args.log)
    bench = None
    status = 0
    try:
        bench = Bench(harness_serial, args.attach)
        recorder.probe_start, recorder.probe_note = bench.snapshot()
        (run_flash if args.mode == "flash" else run_memory)(args, recorder, log, state, bench)
    except KeyboardInterrupt:
        state["interrupted"] = True
        status = 130
    except (SoakError, ProbeError, mailbox.MailboxError, dut_flash.DutFlashError) as error:
        print(f"probe_soak: {error}", file=sys.stderr)
        state["stopped"] = f"refused or failed: {error}"
        status = 2
    # A check that cannot reach the DUT at all: no harness on USB, a J-Link
    # that cannot connect to the image's chip.
    except Exception as error:
        print(f"probe_soak: {error!r}", file=sys.stderr)
        state["stopped"] = f"failed: {error!r}"
        status = 2
    finally:
        if bench is not None:
            recorder.probe_end, recorder.probe_note = bench.snapshot()
            bench.close()
        recorder.finish()
        state["log_events"], state["log_events_suppressed"] = log.events, log.suppressed
        started = state.pop("started")
        state["seconds"] = round(time.monotonic() - started, 3)
        recorder.describe(soak={**recorder.context["soak"], **state})
        line = recorder.line()
        log.write(line)
        log.close()
        print(probe_report.render([line]))
        if state["stopped"]:
            print(f"probe_soak: stopped: {state['stopped']}", file=sys.stderr)
        if state["interrupted"]:
            print("probe_soak: interrupted; the summary is partial", file=sys.stderr)
    return 1 if status == 0 and state["stopped"] else status


if __name__ == "__main__":
    sys.exit(main())
