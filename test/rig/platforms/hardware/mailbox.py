# SPDX-License-Identifier: Apache-2.0 OR MIT

import json
import os
import re
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

from test.rig.platforms.hardware.probe import ProbeError

COMMAND_SYMBOL = "mclw_dut_mailbox_command"
RESULT_SYMBOL = "mclw_dut_mailbox_result"
MAGIC0 = 0x574C434D
COMMAND_MAGIC1 = 0x31444D43
RESULT_MAGIC1 = 0x31534552
VERSION = 1
COMMAND_CAPACITY = 100
RESULT_CAPACITY = 484
COMMAND_WORDS = 32
RESULT_WORDS = 128
RESULT_PREFIX = "MCLW-HIL/1 "
PING = "PING"
TOOLCHAIN_ENV = "METALCLAW_ARM_TOOLCHAIN_DIR"
TOOLCHAIN_NAME = "arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi"
NM = "arm-none-eabi-nm"
NM_TIMEOUT_S = 30.0
DEFAULT_TIMEOUT_S = 30.0
POLL_S = 0.2
HEADER_MISMATCH = "the DUT result header does not match this image"
TORN_RESULT = "torn DUT result record"
TRANSIENT_PROBE_ERRORS = (
    "Failed to open probe",
    "Error handling CMSIS-DAP command Info",
    "Error in the USB access",
)
SYMBOL_LINE = re.compile(r"([0-9a-fA-F]+)\s+\S\s+(\S+)")


class MailboxError(RuntimeError):
    pass


@dataclass(frozen=True)
class Result:
    sequence: int
    line: str


def toolchain_nm(toolchain_dir=None):
    root = str(toolchain_dir or os.environ.get(TOOLCHAIN_ENV, "")).strip()
    if not root:
        raise MailboxError(f"set {TOOLCHAIN_ENV} to the directory holding {TOOLCHAIN_NAME}")
    nm = Path(root) / TOOLCHAIN_NAME / "bin" / NM
    if not nm.is_file():
        raise MailboxError(f"no {NM} at {nm}")
    return nm


def symbol_addresses(elf, names, toolchain_dir=None):
    elf = Path(elf)
    if not elf.is_file():
        raise MailboxError(f"no DUT ELF at {elf}")
    nm = toolchain_nm(toolchain_dir)
    try:
        completed = subprocess.run(
            [str(nm), str(elf)],
            capture_output=True,
            text=True,
            timeout=NM_TIMEOUT_S,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise MailboxError(f"{nm} failed on {elf}: {error}") from error
    if completed.returncode != 0:
        raise MailboxError(f"{nm} exited {completed.returncode}: {completed.stderr.strip()}")
    wanted = tuple(names)
    found = {}
    for line in completed.stdout.splitlines():
        match = SYMBOL_LINE.fullmatch(line.strip())
        if match is not None and match.group(2) in wanted:
            found[match.group(2)] = int(match.group(1), 16)
    missing = [name for name in wanted if name not in found]
    if missing:
        raise MailboxError(f"{elf} does not define {', '.join(missing)}")
    return found


def pack_command(sequence, command):
    try:
        data = command.encode("ascii")
    except UnicodeEncodeError as error:
        raise MailboxError(f"a command must be ASCII: {command!r}") from error
    if not data or len(data) > COMMAND_CAPACITY:
        raise MailboxError(f"a command must be 1..{COMMAND_CAPACITY} ASCII bytes")
    payload = data + b"\0" * (COMMAND_CAPACITY - len(data))
    words = [MAGIC0, COMMAND_MAGIC1, VERSION, COMMAND_CAPACITY, sequence, len(data)]
    words.extend(
        int.from_bytes(payload[index:index + 4], "little")
        for index in range(0, COMMAND_CAPACITY, 4)
    )
    words.append(sequence)
    return words


def decode_result(words):
    if len(words) != RESULT_WORDS:
        raise MailboxError(f"a result record is {RESULT_WORDS} words, got {len(words)}")
    if tuple(words[:4]) != (MAGIC0, RESULT_MAGIC1, VERSION, RESULT_CAPACITY):
        raise MailboxError(HEADER_MISMATCH)
    begin, length, end = words[4], words[5], words[-1]
    if begin != end or length > RESULT_CAPACITY:
        raise MailboxError(TORN_RESULT)
    if begin == 0:
        return None
    payload = b"".join(word.to_bytes(4, "little") for word in words[6:-1])[:length]
    return Result(begin, payload.decode("ascii", "replace").strip())


def parse_result_line(line):
    candidates = [
        text.strip()
        for text in line.splitlines()
        if text.strip().startswith(RESULT_PREFIX)
    ]
    if len(candidates) != 1:
        raise MailboxError(
            f"a result must carry exactly one {RESULT_PREFIX.strip()} line: {line!r}"
        )
    try:
        parsed = json.loads(candidates[0][len(RESULT_PREFIX):])
    except json.JSONDecodeError as error:
        raise MailboxError(f"malformed result line: {candidates[0]!r}") from error
    if not isinstance(parsed, dict):
        raise MailboxError(f"a result line must carry a JSON object: {candidates[0]!r}")
    return parsed


class Mailbox:
    def __init__(self, probe, elf, toolchain_dir=None):
        self.probe = probe
        self.elf = Path(elf)
        addresses = symbol_addresses(
            self.elf, (COMMAND_SYMBOL, RESULT_SYMBOL), toolchain_dir
        )
        self.command_address = addresses[COMMAND_SYMBOL]
        self.result_address = addresses[RESULT_SYMBOL]
        self.last_result = 0
        self.next_command = 1

    def _access(self, call):
        for attempt in range(2):
            try:
                return call()
            except ProbeError as error:
                text = str(error)
                if attempt == 0 and any(
                    marker in text for marker in TRANSIENT_PROBE_ERRORS
                ):
                    time.sleep(POLL_S)
                    continue
                raise MailboxError(text) from error
        raise MailboxError("a bounded probe retry must return or raise")

    def _snapshot(self):
        words = self._access(
            lambda: self.probe.read_words(self.result_address, RESULT_WORDS)
        )
        try:
            return True, decode_result(words)
        except MailboxError as error:
            if str(error) == HEADER_MISMATCH:
                return False, None
            if str(error) == TORN_RESULT:
                return True, None
            raise

    def _wait_ready(self, deadline):
        initialised, _ = self._snapshot()
        while not initialised and time.monotonic() < deadline:
            time.sleep(POLL_S)
            initialised, _ = self._snapshot()
        if not initialised:
            raise MailboxError("the DUT mailbox was not initialised before the deadline")

    def wait_ready(self, timeout=DEFAULT_TIMEOUT_S):
        self._wait_ready(time.monotonic() + timeout)

    def read_result(self):
        initialised, result = self._snapshot()
        if not initialised or result is None or result.sequence <= self.last_result:
            return None
        self.last_result = result.sequence
        return result.line

    def write_command(self, line):
        sequence = self.next_command
        self.next_command += 1
        words = pack_command(sequence, line)
        self._access(
            lambda: self.probe.write_words(self.command_address, words)
        )
        return sequence

    def command(self, line, timeout=DEFAULT_TIMEOUT_S):
        deadline = time.monotonic() + timeout
        self._wait_ready(deadline)
        _, before = self._snapshot()
        if before is not None and before.sequence > self.last_result:
            self.last_result = before.sequence
        sequence = self.write_command(line)
        while time.monotonic() < deadline:
            found = self.read_result()
            if found is not None:
                return parse_result_line(found)
            time.sleep(POLL_S)
        raise MailboxError(f"no result after command sequence {sequence}: {line!r}")

    def ping(self, timeout=DEFAULT_TIMEOUT_S):
        return self.command(PING, timeout)
