# SPDX-License-Identifier: Apache-2.0 OR MIT
# Every way a rig session ends stops its Renode: a normal teardown, an
# interrupt in the middle of starting or stopping, SIGTERM or SIGHUP, an
# interpreter exit, and a Renode that ignores quit and SIGTERM.

import os
import signal
import subprocess
import sys
import time
import types

import pytest

from test.rig.renode import bench


def a_bench(tmp_path):
    b = bench.Bench(tmp_path, bench.LAYOUTS["metalclaw_pico_rp2"], tmp_path / "h.uf2", tmp_path, tmp_path,
                    tmp_path / "bridge.sock", 6)
    b.log = (tmp_path / "renode.log").open("w")
    return b


# A Renode in its own session, as the bench starts it, that ignores SIGTERM.
def stubborn_renode():
    return subprocess.Popen([sys.executable, "-c", "import signal, time\n"
                             "signal.signal(signal.SIGTERM, signal.SIG_IGN)\nprint('up', flush=True)\n"
                             "time.sleep(60)\n"], stdout=subprocess.PIPE, start_new_session=True)


@pytest.fixture
def short_graces(monkeypatch):
    monkeypatch.setattr(bench, "QUIT_GRACE_S", 0.5)
    monkeypatch.setattr(bench, "TERM_GRACE_S", 0.5)


@pytest.fixture
def default_exit_signals():
    saved = {n: signal.getsignal(n) for n in bench.EXIT_SIGNALS}
    for n in bench.EXIT_SIGNALS:
        signal.signal(n, signal.SIG_DFL)
    yield
    for n, handler in saved.items():
        signal.signal(n, handler)


def test_a_renode_that_ignores_sigterm_is_killed_after_its_grace(tmp_path, short_graces):
    b = a_bench(tmp_path)
    b.process = stubborn_renode()
    assert b.process.stdout.readline() == b"up\n"
    started = time.monotonic()
    b.stop()
    assert b.process.returncode == -signal.SIGKILL
    assert time.monotonic() - started < 10
    assert (tmp_path / "renode.log").read_text() == (
        "rig: Renode still running at the end of the session\n"
        "rig: Renode's monitor did not take quit; sending SIGTERM\n"
        "rig: Renode outlived SIGTERM by 0 s; sending SIGKILL\n")


def test_a_renode_that_ignores_quit_gets_sigterm(tmp_path, short_graces, monkeypatch):
    class Monitor:
        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def sendall(self, data):
            sent.append(data)

    sent = []
    monkeypatch.setattr(bench.socket, "create_connection", lambda *args, **kwargs: Monitor())
    b = a_bench(tmp_path)
    b.monitor_port = 1240
    b.process = subprocess.Popen(["sleep", "60"], start_new_session=True)
    b.stop()
    assert sent == [b"quit\n"]
    assert b.process.returncode == -signal.SIGTERM
    assert "rig: Renode did not quit within 0 s; sending SIGTERM\n" in (tmp_path / "renode.log").read_text()


def test_the_whole_renode_process_group_is_signalled(tmp_path, short_graces):
    b = a_bench(tmp_path)
    b.process = subprocess.Popen(["sh", "-c", "sleep 60 & echo $!; wait"], stdout=subprocess.PIPE,
                                 start_new_session=True)
    child = int(b.process.stdout.readline())
    b.stop()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            os.kill(child, 0)
        except ProcessLookupError:
            return
        time.sleep(0.05)
    os.kill(child, signal.SIGKILL)
    pytest.fail("Renode's child outlived the stop")


class FailingCable:
    target = "usbip://127.0.0.1:3246/1-0"

    def __init__(self, error=RuntimeError("usbip detach failed")):
        self.error = error

    def stop(self):
        raise self.error


def test_a_cable_that_cannot_detach_still_lets_renode_stop(tmp_path, short_graces):
    b = a_bench(tmp_path)
    b.cable = FailingCable()
    b.process = subprocess.Popen(["sleep", "60"], start_new_session=True)
    with pytest.raises(RuntimeError, match="usbip detach failed"):
        b.stop()
    assert b.process.returncode is not None
    assert b.cable is None and b.log is None


def test_an_interrupt_while_stopping_waits_until_renode_is_down(tmp_path, short_graces):
    class InterruptedCable:
        target = "usbip://127.0.0.1:3246/1-0"

        def stop(self):
            # A second Ctrl-C, or timeout's SIGINT, part way through.
            signal.raise_signal(signal.SIGINT)
            stopped.append(True)

    stopped = []
    # A session started in the background starts with SIGINT ignored, not
    # with Python's handler; the stop puts back whichever it found.
    before = signal.getsignal(signal.SIGINT)
    b = a_bench(tmp_path)
    b.cable = InterruptedCable()
    b.process = subprocess.Popen(["sleep", "60"], start_new_session=True)
    with pytest.raises(KeyboardInterrupt, match="SIGINT while the bench stopped"):
        b.stop()
    assert stopped == [True]
    assert b.process.returncode is not None
    assert signal.getsignal(signal.SIGINT) is before


def test_an_interrupt_while_starting_stops_the_renode_already_launched(tmp_path, short_graces, monkeypatch,
                                                                       default_exit_signals):
    for name in ("renode", "scripts/multi-node/metalclaw_harness_rp2_native.resc", "roms/bootrom-combined.bin",
                 "h.uf2"):
        (tmp_path / name).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / name).write_text("")
    b = bench.Bench(tmp_path, bench.LAYOUTS["metalclaw_pico_rp2"], tmp_path / "h.uf2", tmp_path / "roms",
                    tmp_path / "work", tmp_path / "bridge.sock", 6)
    monkeypatch.setattr(bench, "MONITOR_PORT", 1)
    monkeypatch.setattr(bench, "USBIP_PORT", 1)
    monkeypatch.setattr(bench, "_listening", lambda port: False)
    monkeypatch.setattr(bench.uf2, "flash_contents", lambda data, chip: b"harness")
    monkeypatch.setattr(bench.storage, "with_storage", lambda image: image)
    monkeypatch.setattr(b, "_rom", lambda chip, name: tmp_path / name)
    real, launched = subprocess.Popen, []

    def popen(argv, **kwargs):
        launched.append(real(["sleep", "60"], start_new_session=True))
        return launched[-1]

    def interrupted(port, timeout):
        assert signal.getsignal(signal.SIGTERM) is bench._interrupt
        raise KeyboardInterrupt

    monkeypatch.setattr(bench.subprocess, "Popen", popen)
    monkeypatch.setattr(bench, "_wait_port", interrupted)
    with pytest.raises(KeyboardInterrupt):
        b.start()
    assert launched[0].returncode is not None
    assert signal.getsignal(signal.SIGTERM) is signal.SIG_DFL
    assert "rig: Renode still running at the end of the session\n" in (tmp_path / "work" / "renode.log").read_text()


def test_a_running_bench_is_stopped_at_interpreter_exit_and_released_after(tmp_path, monkeypatch,
                                                                           default_exit_signals):
    registered = []
    monkeypatch.setattr(bench, "atexit", types.SimpleNamespace(register=registered.append,
                                                               unregister=registered.remove))
    b = a_bench(tmp_path)
    b._hold_exits()
    assert registered == [b.stop]
    b.stop()
    assert registered == []


def test_sigterm_and_sighup_end_a_running_session_as_an_interrupt(tmp_path, default_exit_signals):
    b = a_bench(tmp_path)
    b._hold_exits()
    try:
        for number in (signal.SIGTERM, signal.SIGHUP):
            assert signal.getsignal(number) is bench._interrupt
            with pytest.raises(KeyboardInterrupt, match=signal.Signals(number).name):
                signal.raise_signal(number)
    finally:
        b.stop()
    assert all(signal.getsignal(n) is signal.SIG_DFL for n in bench.EXIT_SIGNALS)


def test_a_signal_the_session_already_handles_is_left_alone(tmp_path, default_exit_signals):
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    b = a_bench(tmp_path)
    b._hold_exits()
    assert signal.getsignal(signal.SIGHUP) is signal.SIG_IGN
    assert signal.getsignal(signal.SIGTERM) is bench._interrupt
    b.stop()
    assert signal.getsignal(signal.SIGHUP) is signal.SIG_IGN


def test_renode_dies_with_the_rig_when_setpriv_is_there(monkeypatch):
    monkeypatch.setattr(bench, "SETPRIV", "/usr/bin/setpriv")
    argv = bench.renode_argv(bench.Path("/r/renode"), 1240, ["include @x", "start"])
    assert argv == ["/usr/bin/setpriv", "--pdeathsig", "KILL", "/r/renode", "--disable-gui", "--plain", "--port",
                    "1240", "-e", "include @x", "-e", "start"]
    monkeypatch.setattr(bench, "SETPRIV", None)
    assert bench.renode_argv(bench.Path("/r/renode"), 1240, [])[0] == "/r/renode"


@pytest.mark.skipif(bench.SETPRIV is None, reason="no setpriv on this host")
def test_a_rig_killed_outright_takes_its_renode_with_it(tmp_path):
    # The rig, killed with SIGKILL: no handler, no atexit, only the parent
    # death signal Renode was started with.
    rig = subprocess.Popen([sys.executable, "-c", "import subprocess, sys, time\n"
                            f"p = subprocess.Popen([{bench.SETPRIV!r}, '--pdeathsig', 'KILL', 'sleep', '60'], "
                            "start_new_session=True)\nprint(p.pid, flush=True)\ntime.sleep(60)\n"],
                           stdout=subprocess.PIPE)
    renode = int(rig.stdout.readline())
    # Once setpriv has set the signal and exec'd.
    deadline = time.monotonic() + 5
    while comm(renode) != "sleep" and time.monotonic() < deadline:
        time.sleep(0.01)
    rig.kill()
    rig.wait()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if comm(renode) is None:
            return
        time.sleep(0.05)
    os.kill(renode, signal.SIGKILL)
    pytest.fail("the Renode stand-in outlived its rig")


# A process's name, or None once it is gone or a zombie.
def comm(pid: int) -> str | None:
    try:
        stat = open(f"/proc/{pid}/stat").read()
    except OSError:
        return None
    name, state = stat[stat.index("(") + 1:stat.rindex(")")], stat[stat.rindex(")") + 2]
    return None if state in "ZX" else name


def test_a_signal_handled_outside_python_is_left_alone_while_stopping(tmp_path, monkeypatch):
    # getsignal() answers None for a handler set outside Python, and
    # signal.signal() refuses None: restoring one raised a TypeError in
    # place of the interrupt once Renode was down (a runner session on the
    # emulator, interrupted with SIGINT).
    real_getsignal, touched = signal.getsignal, []
    monkeypatch.setattr(bench.signal, "getsignal",
                        lambda n: None if n == signal.SIGHUP else real_getsignal(n))
    real_signal = signal.signal
    monkeypatch.setattr(bench.signal, "signal", lambda n, h: touched.append(n) or real_signal(n, h))
    b = a_bench(tmp_path)
    b.stop()
    assert signal.SIGHUP not in touched and signal.SIGINT in touched
