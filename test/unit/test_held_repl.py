# SPDX-License-Identifier: Apache-2.0 OR MIT

import pytest

from test.rig.platforms.hardware import platform


class FakePort:
    def __init__(self, repl):
        self.repl = repl


class FakeRepl:
    opened = []

    def __init__(self):
        self.dead = False
        self.fail_run = False
        self.runs = []
        self.closed = False
        self.port = FakePort(self)
        FakeRepl.opened.append(self)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.closed = True

    def run(self, code, timeout=5.0):
        if self.fail_run:
            raise RuntimeError("the port died mid-call")
        self.runs.append(code)
        return "ok"


@pytest.fixture
def harness(monkeypatch):
    FakeRepl.opened = []
    monkeypatch.setattr(platform.Harness, "repl", lambda self: FakeRepl())
    monkeypatch.setattr(platform, "_port_alive", lambda port: not port.repl.dead)
    return platform.Harness("0123456789ABCDEF")


def test_one_repl_serves_every_call(harness):
    harness.run("a")
    harness.run("b")
    assert len(FakeRepl.opened) == 1
    assert FakeRepl.opened[0].runs == ["a", "b"]


def test_a_dead_held_repl_is_reopened_before_the_code_goes_out(harness):
    harness.run("a")
    FakeRepl.opened[0].dead = True
    assert harness.run("b") == "ok"
    first, second = FakeRepl.opened
    assert first.closed and first.runs == ["a"] and second.runs == ["b"]


def test_code_that_failed_mid_call_is_not_sent_again(harness):
    harness.run("a")
    FakeRepl.opened[0].fail_run = True
    with pytest.raises(RuntimeError, match="mid-call"):
        harness.run("b")
    assert len(FakeRepl.opened) == 1 and FakeRepl.opened[0].closed
    harness.run("c")
    assert FakeRepl.opened[1].runs == ["c"]


def test_a_port_whose_node_was_replaced_is_dead(tmp_path):
    class Port:
        def __init__(self, path):
            self.port = str(path)
            self.file = open(path, "rb")

        def fileno(self):
            return self.file.fileno()

        def reset_input_buffer(self):
            pass

    node = tmp_path / "ttyACM0"
    node.write_bytes(b"")
    port = Port(node)
    assert platform._port_alive(port)
    node.unlink()
    node.write_bytes(b"")
    assert not platform._port_alive(port)
    port.file.close()
