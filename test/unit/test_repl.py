# SPDX-License-Identifier: Apache-2.0 OR MIT

import pytest

from test.rig.platforms.hardware import repl as repl_module


RAW_PROMPT = b"\r\nraw REPL; CTRL-B to exit\r\n>"


class Clock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class ScriptedSerial:
    def __init__(self, clock, prompt, delay, response=b"OKattached\r\n\x04\x04>"):
        self.clock = clock
        self.prompt = prompt
        self.delay = delay
        self.response = response
        self.pending = []
        self.events = []
        self.entry_ready_at = 0.0
        self.closed = False

    def write(self, data):
        self.events.append(("write", data, self.clock.now))
        if data == b"\x01":
            for index, chunk in enumerate(self.prompt):
                self.entry_ready_at = self.clock.now + self.delay + index * .05
                self.pending.append((self.entry_ready_at, chunk))
        elif data.endswith(b"\x04"):
            ready = max(self.clock.now, self.entry_ready_at) + .1
            self.pending.append((ready, self.response))

    def read(self, count):
        self.clock.sleep(.05)
        ready = [chunk for due, chunk in self.pending if due <= self.clock.now]
        self.pending = [(due, chunk) for due, chunk in self.pending if due > self.clock.now]
        return b"".join(ready)

    def reset_input_buffer(self):
        self.events.append(("flush", self.clock.now))
        self.pending = [(due, chunk) for due, chunk in self.pending if due > self.clock.now]

    def close(self):
        self.closed = True


def make_repl(monkeypatch, prompt=(RAW_PROMPT,), delay=0, **kwargs):
    clock = Clock()
    port = ScriptedSerial(clock, prompt, delay, **kwargs)
    monkeypatch.setattr(repl_module.glob, "glob", lambda pattern: ["test-only-serial"])
    monkeypatch.setattr(repl_module.serial, "Serial", lambda *a, **kw: port)
    monkeypatch.setattr(repl_module.time, "monotonic", clock.monotonic)
    monkeypatch.setattr(repl_module.time, "sleep", clock.sleep)
    return repl_module.Repl("fixture"), clock, port


@pytest.mark.parametrize("delay", [0, .75])
@pytest.mark.parametrize("prompt", [(RAW_PROMPT,), (b"\r\nraw RE", b"PL; CTRL-B to exit\r", b"\n", b">")])
def test_command_waits_for_complete_raw_entry(monkeypatch, delay, prompt):
    repl, clock, port = make_repl(monkeypatch, prompt, delay)
    with repl:
        assert clock.now >= port.entry_ready_at
        assert repl.run("print('attached')") == "attached\r\n"
    assert port.closed
    assert port.events[-1][1] == b"\x02"


@pytest.mark.parametrize("prompt", [(), (b"raw REPL; CTRL-B to exit\r\n",), (b"\r\n>>> ",)])
def test_entry_timeout_is_bounded_and_closes_port(monkeypatch, prompt):
    repl, clock, port = make_repl(monkeypatch, prompt)
    with pytest.raises(RuntimeError, match="raw REPL entry prompt timed out"):
        repl.__enter__()
    assert 5.0 <= clock.now < 5.2
    assert port.closed


def test_entry_consumes_friendly_output_before_raw_prompt(monkeypatch):
    repl, clock, port = make_repl(monkeypatch, (b"old friendly prompt\r\n>>> ", RAW_PROMPT), .75)
    with repl:
        assert repl.run("print('attached')") == "attached\r\n"


def test_stale_input_is_flushed_before_entering_raw_mode(monkeypatch):
    repl, clock, port = make_repl(monkeypatch)
    port.pending.append((0, b"stale command output"))
    with repl:
        assert port.events[0] == ("flush", 0)
        assert repl.run("print('attached')") == "attached\r\n"


def test_application_reply_prefix_remains_strict(monkeypatch):
    repl, clock, port = make_repl(monkeypatch, delay=.75, response=b"junkOKattached\r\n\x04\x04>")
    with repl:
        with pytest.raises(RuntimeError, match="raw REPL did not accept the code"):
            repl.run("print('attached')")
