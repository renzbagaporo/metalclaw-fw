# SPDX-License-Identifier: Apache-2.0 OR MIT

import math
import os
import re
import subprocess
from pathlib import Path

DEFAULT_CHIP = "RP2350_Core0"
DEFAULT_SPEED_KHZ = 1000
PROBE_RS_ENV = "METALCLAW_PROBE_RS"
FORBIDDEN_FLAGS = ("--connect-under-reset",)
DOWNLOAD_TIMEOUT_S = 180.0
DOWNLOAD_TIMEOUT_ENV = "METALCLAW_PROBE_DOWNLOAD_TIMEOUT_S"
RESET_TIMEOUT_S = 30.0
RESET_TIMEOUT_ENV = "METALCLAW_PROBE_RESET_TIMEOUT_S"
_FROM_ENVIRONMENT = object()
ACCESS_TIMEOUT_S = 30.0
ADDRESSED_LINE = re.compile(r"\s*([0-9a-fA-F]{8}):\s*(.*?)\s*")
WORD = re.compile(r"[0-9a-fA-F]{8}")


class ProbeError(RuntimeError):
    def __init__(self, message, argv=(), returncode=None, stdout="", stderr=""):
        super().__init__(message)
        self.argv = tuple(argv)
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _text(output):
    return output.decode(errors="replace") if isinstance(output, bytes) else output or ""


def _deadline(operation, timeout, environment, default):
    value = os.environ.get(environment, default) if timeout is _FROM_ENVIRONMENT else timeout
    invalid = f"invalid {operation} timeout {value!r}; expected positive finite seconds"
    try:
        seconds = float(value)
    except (TypeError, ValueError, OverflowError) as error:
        raise ProbeError(invalid) from error
    if not math.isfinite(seconds) or seconds <= 0:
        raise ProbeError(invalid)
    return seconds


def decode_words(output, count, address):
    values = []
    expected = address
    addressed = False
    for line in output.splitlines():
        match = ADDRESSED_LINE.fullmatch(line)
        if match is None:
            continue
        addressed = True
        current = int(match.group(1), 16)
        for token in match.group(2).split():
            if WORD.fullmatch(token) is None or current != expected:
                raise ProbeError(f"unexpected probe-rs RAM read output: {line!r}")
            values.append(int(token, 16))
            current += 4
            expected += 4
    if not addressed:
        tokens = output.split()
        if any(WORD.fullmatch(token) is None for token in tokens):
            raise ProbeError(f"unexpected probe-rs RAM read output: {output!r}")
        values = [int(token, 16) for token in tokens]
    if len(values) != count:
        raise ProbeError(f"probe-rs returned {len(values)} words, expected {count}")
    return values


class ProbeRs:
    def __init__(
        self,
        probe_id,
        chip_description,
        speed_khz=DEFAULT_SPEED_KHZ,
        chip=DEFAULT_CHIP,
        executable=None,
    ):
        self.probe_id = probe_id
        # None uses probe-rs's built-in description of the chip.
        self.chip_description = None if chip_description is None else Path(chip_description)
        self.speed_khz = int(speed_khz)
        self.chip = chip
        self.executable = str(executable or os.environ.get(PROBE_RS_ENV) or "probe-rs")
        if not self.probe_id:
            raise ProbeError("a probe id is required; the rig never scans for a probe")
        if self.chip_description is not None and not self.chip_description.is_file():
            raise ProbeError(f"no chip description at {self.chip_description}")
        if self.speed_khz <= 0:
            raise ProbeError(f"invalid SWD speed {self.speed_khz} kHz")

    def _argv(self, operation, *arguments):
        description = [] if self.chip_description is None else ["--chip-description-path", str(self.chip_description)]
        argv = [
            self.executable,
            operation,
            *description,
            "--chip",
            self.chip,
            "--probe",
            self.probe_id,
            "--protocol",
            "swd",
            "--speed",
            str(self.speed_khz),
            *arguments,
        ]
        for flag in FORBIDDEN_FLAGS:
            if flag in argv:
                raise ProbeError(f"{flag} is never used against this DUT")
        return argv

    def _run(self, argv, timeout):
        environment = {
            name: value
            for name, value in os.environ.items()
            if not name.startswith("PROBE_RS_")
        }
        try:
            completed = subprocess.run(
                argv,
                capture_output=True,
                text=True,
                timeout=timeout,
                check=False,
                env=environment,
            )
        except FileNotFoundError as error:
            raise ProbeError(f"cannot run {self.executable}: {error}", argv) from error
        except subprocess.TimeoutExpired as error:
            stdout, stderr = _text(error.stdout), _text(error.stderr)
            raise ProbeError(
                f"{' '.join(argv)} timed out after {timeout} s: {(stderr or stdout).strip()}",
                argv,
                None,
                stdout,
                stderr,
            ) from error
        if completed.returncode != 0:
            raise ProbeError(
                f"{' '.join(argv)} exited {completed.returncode}: "
                f"{(completed.stderr or completed.stdout).strip()}",
                argv,
                completed.returncode,
                completed.stdout,
                completed.stderr,
            )
        return completed.stdout

    def download(self, image, verify=True, reset=True, timeout=_FROM_ENVIRONMENT):
        image = Path(image)
        if not image.is_file():
            raise ProbeError(f"no DUT image at {image}")
        timeout = _deadline("download", timeout, DOWNLOAD_TIMEOUT_ENV, DOWNLOAD_TIMEOUT_S)
        arguments = []
        if image.suffix.lower() == ".hex":
            arguments.extend(("--binary-format", "hex"))
        if verify:
            arguments.append("--verify")
        if reset:
            arguments.append("--reset")
        arguments.append(str(image))
        return self._run(self._argv("download", *arguments), timeout)

    def reset(self, timeout=_FROM_ENVIRONMENT):
        timeout = _deadline("reset", timeout, RESET_TIMEOUT_ENV, RESET_TIMEOUT_S)
        return self._run(self._argv("reset"), timeout)

    def read_words(self, address, count, timeout=ACCESS_TIMEOUT_S):
        if address <= 0:
            raise ProbeError(f"invalid RAM read address {address:#x}")
        if count <= 0:
            raise ProbeError("a RAM read needs a positive word count")
        output = self._run(
            self._argv("read", "b32", hex(address), str(count)), timeout
        )
        return decode_words(output, count, address)

    def write_words(self, address, words, timeout=ACCESS_TIMEOUT_S):
        values = list(words)
        if address <= 0:
            raise ProbeError(f"invalid RAM write address {address:#x}")
        if not values:
            raise ProbeError("a RAM write needs at least one word")
        literals = [f"0x{value & 0xFFFFFFFF:08x}" for value in values]
        self._run(self._argv("write", "b32", hex(address), *literals), timeout)
