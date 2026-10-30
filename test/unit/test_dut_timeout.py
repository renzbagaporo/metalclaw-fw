# SPDX-License-Identifier: Apache-2.0 OR MIT

import json
from types import SimpleNamespace

import pytest

from test.rig.platforms.hardware import mailbox as mailbox_module
from test.rig.platforms.hardware.platform import (
    DUT_MAILBOX_TIMEOUT_ENV,
    DUT_MAILBOX_TIMEOUT_S,
    Dut,
)
from test.rig.platforms.hardware.probe import ProbeError


@pytest.fixture(autouse=True)
def isolated_environment(tmp_path, monkeypatch):
    for name in ("HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "TMPDIR"):
        directory = tmp_path / name.lower()
        directory.mkdir()
        monkeypatch.setenv(name, str(directory))
    monkeypatch.delenv(DUT_MAILBOX_TIMEOUT_ENV, raising=False)


class RecordingMailbox:
    def __init__(self):
        self.calls = []
        self.after_ready = lambda: None

    def wait_ready(self, timeout):
        self.calls.append(("ready", timeout))
        self.after_ready()

    def ping(self, timeout):
        self.calls.append(("ping", timeout))
        return {"status": "ok"}

    def command(self, line, timeout):
        self.calls.append((line, timeout))
        return {"status": "ok"}


def make_dut(mailbox):
    dut = object.__new__(Dut)
    dut.mailbox = mailbox
    return dut


def invoke(dut, method, **kwargs):
    return dut.ping(**kwargs) if method == "ping" else dut.command("PING", **kwargs)


def expected_calls(method, timeout):
    return [("ready", timeout), ("ping", timeout)] if method == "ping" else [("PING", timeout)]


@pytest.mark.parametrize("method", ["ping", "command"])
def test_public_default_budget(method):
    mailbox = RecordingMailbox()
    assert invoke(make_dut(mailbox), method) == {"status": "ok"}
    assert DUT_MAILBOX_TIMEOUT_S == 10.0
    assert mailbox.calls == expected_calls(method, 10.0)


@pytest.mark.parametrize("method", ["ping", "command"])
@pytest.mark.parametrize("value,expected", [("120", 120.0), (" 2.5 ", 2.5)])
def test_public_environment_budget(method, value, expected, monkeypatch):
    mailbox = RecordingMailbox()
    monkeypatch.setenv(DUT_MAILBOX_TIMEOUT_ENV, value)
    invoke(make_dut(mailbox), method)
    assert mailbox.calls == expected_calls(method, expected)


@pytest.mark.parametrize("method", ["ping", "command"])
@pytest.mark.parametrize("environment,explicit", [("120", 10.0), ("malformed", 3.0)])
def test_explicit_budget_has_priority(method, environment, explicit, monkeypatch):
    mailbox = RecordingMailbox()
    monkeypatch.setenv(DUT_MAILBOX_TIMEOUT_ENV, environment)
    invoke(make_dut(mailbox), method, timeout=explicit)
    assert mailbox.calls == expected_calls(method, explicit)


@pytest.mark.parametrize("method", ["ping", "command"])
@pytest.mark.parametrize("value", ["", "bad", "0", "-1", "nan", "inf", "-inf", "1e309"])
def test_invalid_environment_fails_before_io(method, value, monkeypatch):
    mailbox = RecordingMailbox()
    monkeypatch.setenv(DUT_MAILBOX_TIMEOUT_ENV, value)
    with pytest.raises(mailbox_module.MailboxError, match="expected positive finite seconds"):
        invoke(make_dut(mailbox), method)
    assert mailbox.calls == []


@pytest.mark.parametrize("method", ["ping", "command"])
@pytest.mark.parametrize("value", [0, -1, None, float("nan"), float("inf"), 10**1000])
def test_invalid_explicit_budget_fails_before_io(method, value, monkeypatch):
    mailbox = RecordingMailbox()
    monkeypatch.setenv(DUT_MAILBOX_TIMEOUT_ENV, "malformed")
    with pytest.raises(mailbox_module.MailboxError, match="expected positive finite seconds"):
        invoke(make_dut(mailbox), method, timeout=value)
    assert mailbox.calls == []


@pytest.mark.parametrize("method", ["ping", "command"])
def test_environment_is_read_for_each_public_call(method, monkeypatch):
    mailbox = RecordingMailbox()
    dut = make_dut(mailbox)
    for value in ("120", "240"):
        monkeypatch.setenv(DUT_MAILBOX_TIMEOUT_ENV, value)
        invoke(dut, method)
    assert mailbox.calls == expected_calls(method, 120.0) + expected_calls(method, 240.0)


def test_ping_resolves_once_for_both_existing_stages(monkeypatch):
    mailbox = RecordingMailbox()
    monkeypatch.setenv(DUT_MAILBOX_TIMEOUT_ENV, "120")
    mailbox.after_ready = lambda: monkeypatch.setenv(DUT_MAILBOX_TIMEOUT_ENV, "malformed")
    make_dut(mailbox).ping()
    assert mailbox.calls == [("ready", 120.0), ("ping", 120.0)]


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, duration):
        self.now += duration


class SlowProbe:
    command_address = 0x20000100
    result_address = 0x20000200

    def __init__(self, clock):
        self.clock = clock
        self.reply = True
        self.read_error = None
        self.written = False
        self.polls = 0
        self.events = []

    def read_words(self, address, count):
        assert address == self.result_address
        assert count == 128
        self.events.append("poll" if self.written else "prepare_read")
        if self.read_error:
            raise self.read_error
        self.clock.sleep(13.0)
        if self.written:
            self.polls += 1
        replied = self.written and self.reply
        payload = ("MCLW-HIL/1 " + json.dumps({
            "test": "test_ping" if replied else "test_boot", "status": "ok", "image": "smoke",
        }) + "\r\n").encode("ascii")
        data = payload.ljust(484, b"\0")
        sequence = 2 if replied else 1
        return [0x574C434D, 0x31534552, 1, 484, sequence, len(payload)] + [
            int.from_bytes(data[index:index + 4], "little") for index in range(0, 484, 4)
        ] + [sequence]

    def write_words(self, address, words):
        assert address == self.command_address
        assert len(words) == 32
        assert words[:6] == [0x574C434D, 0x31444D43, 1, 100, 1, 4]
        assert words[-1] == 1
        payload = b"".join(word.to_bytes(4, "little") for word in words[6:-1])
        assert payload == b"PING" + bytes(96)
        self.events.append("write")
        self.clock.sleep(11.0)
        self.written = True


@pytest.fixture
def slow_dut(tmp_path, monkeypatch):
    clock = FakeClock()
    probe = SlowProbe(clock)
    monkeypatch.setattr(mailbox_module, "time", clock)
    monkeypatch.setattr(mailbox_module, "symbol_addresses", lambda *args: {
        mailbox_module.COMMAND_SYMBOL: probe.command_address,
        mailbox_module.RESULT_SYMBOL: probe.result_address,
    })
    image = tmp_path / "test_only.elf"
    image.write_bytes(b"test-only image")
    mailbox = mailbox_module.Mailbox(probe, image)
    return SimpleNamespace(dut=make_dut(mailbox), clock=clock, probe=probe)


@pytest.mark.parametrize("method", ["ping", "command"])
def test_default_budget_still_expires_during_preparation(method, slow_dut):
    with pytest.raises(mailbox_module.MailboxError, match="no result after command sequence 1: 'PING'"):
        invoke(slow_dut.dut, method)
    assert slow_dut.probe.written
    assert slow_dut.clock.now > 10.0
    assert slow_dut.probe.polls == 0
    assert slow_dut.probe.events[-1] == "write"


@pytest.mark.parametrize("method", ["ping", "command"])
def test_configured_budget_polls_and_parses_real_mailbox_result(method, slow_dut, monkeypatch):
    monkeypatch.setenv(DUT_MAILBOX_TIMEOUT_ENV, "120")
    assert invoke(slow_dut.dut, method) == {"test": "test_ping", "status": "ok", "image": "smoke"}
    assert slow_dut.probe.polls == 1
    assert 10.0 < slow_dut.clock.now < 120.0


@pytest.mark.parametrize("method", ["ping", "command"])
def test_configured_budget_still_expires_without_new_reply(method, slow_dut, monkeypatch):
    slow_dut.probe.reply = False
    monkeypatch.setenv(DUT_MAILBOX_TIMEOUT_ENV, "120")
    with pytest.raises(mailbox_module.MailboxError, match="no result after command sequence 1: 'PING'"):
        invoke(slow_dut.dut, method)
    assert slow_dut.probe.polls > 0
    assert 120.0 <= slow_dut.clock.now < 150.0


@pytest.mark.parametrize("method", ["ping", "command"])
def test_explicit_short_budget_remains_enforced_with_environment(method, slow_dut, monkeypatch):
    monkeypatch.setenv(DUT_MAILBOX_TIMEOUT_ENV, "120")
    with pytest.raises(mailbox_module.MailboxError, match="no result after command sequence 1: 'PING'"):
        invoke(slow_dut.dut, method, timeout=10.0)
    assert slow_dut.probe.polls == 0


@pytest.mark.parametrize("method", ["ping", "command"])
def test_probe_failure_is_not_hidden_by_configured_budget(method, slow_dut, monkeypatch):
    monkeypatch.setenv(DUT_MAILBOX_TIMEOUT_ENV, "120")
    slow_dut.probe.read_error = ProbeError("permanent probe failure")
    with pytest.raises(mailbox_module.MailboxError, match="permanent probe failure") as caught:
        invoke(slow_dut.dut, method)
    assert isinstance(caught.value.__cause__, ProbeError)
    assert slow_dut.probe.events == ["prepare_read"]
