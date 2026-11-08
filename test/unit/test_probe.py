# SPDX-License-Identifier: Apache-2.0 OR MIT

import json
import os
import subprocess
import sys

import pytest

from test.rig.platforms.hardware.probe import (
    ACCESS_TIMEOUT_ENV,
    ACCESS_TIMEOUT_S,
    DOWNLOAD_TIMEOUT_ENV,
    DOWNLOAD_TIMEOUT_S,
    RESET_TIMEOUT_ENV,
    RESET_TIMEOUT_S,
    ProbeError,
    ProbeRs,
)


@pytest.fixture
def probe_files(tmp_path, monkeypatch):
    for name in ("HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "TMPDIR"):
        directory = tmp_path / name.lower()
        directory.mkdir()
        monkeypatch.setenv(name, str(directory))
    monkeypatch.setenv("PYTHONNOUSERSITE", "1")
    monkeypatch.delenv(DOWNLOAD_TIMEOUT_ENV, raising=False)
    monkeypatch.delenv(RESET_TIMEOUT_ENV, raising=False)
    monkeypatch.delenv(ACCESS_TIMEOUT_ENV, raising=False)
    description = tmp_path / "chip.yaml"
    description.write_text("test-only chip description\n")
    image = tmp_path / "image.elf"
    image.write_bytes(b"test-only image")
    executable = tmp_path / "fake_probe_rs"
    probe = ProbeRs("test-probe-id", description, executable=executable)
    return probe, image, executable


@pytest.fixture
def runs(monkeypatch):
    calls = []

    def run(argv, **kwargs):
        calls.append((argv, kwargs))
        stdout = "12345678" if argv[1] == "read" else "download complete\n"
        return subprocess.CompletedProcess(argv, 0, stdout, "")

    monkeypatch.setattr(subprocess, "run", run)
    return calls


def test_download_default_deadline(probe_files, runs):
    probe, image, _ = probe_files
    assert probe.download(image) == "download complete\n"
    assert len(runs) == 1
    assert runs[0][1]["timeout"] == DOWNLOAD_TIMEOUT_S == 180.0


@pytest.mark.parametrize("value,expected", [("900", 900.0), ("0.25", 0.25), (" 5.5 ", 5.5), ("1e3", 1000.0)])
def test_download_environment_deadline(probe_files, runs, monkeypatch, value, expected):
    probe, image, _ = probe_files
    monkeypatch.setenv(DOWNLOAD_TIMEOUT_ENV, value)
    probe.download(image)
    assert runs[0][1]["timeout"] == expected


@pytest.mark.parametrize("value", ["", " ", "nope", "0", "-1", "nan", "inf", "-inf", "1e309"])
def test_download_rejects_invalid_environment_deadline(probe_files, runs, monkeypatch, value):
    probe, image, _ = probe_files
    monkeypatch.setenv(DOWNLOAD_TIMEOUT_ENV, value)
    with pytest.raises(ProbeError, match="expected positive finite seconds"):
        probe.download(image)
    assert not runs


@pytest.mark.parametrize("environment,explicit", [("900", 180.0), ("malformed", 2.5)])
def test_explicit_download_deadline_has_priority(probe_files, runs, monkeypatch, environment, explicit):
    probe, image, _ = probe_files
    monkeypatch.setenv(DOWNLOAD_TIMEOUT_ENV, environment)
    probe.download(image, timeout=explicit)
    assert runs[0][1]["timeout"] == explicit


@pytest.mark.parametrize("value", [0, -1, float("nan"), float("inf"), None, 10**1000])
def test_download_rejects_invalid_explicit_deadline(probe_files, runs, monkeypatch, value):
    probe, image, _ = probe_files
    monkeypatch.setenv(DOWNLOAD_TIMEOUT_ENV, "malformed")
    with pytest.raises(ProbeError, match="expected positive finite seconds"):
        probe.download(image, timeout=value)
    assert not runs


def test_download_reads_environment_for_each_call(probe_files, runs, monkeypatch):
    probe, image, _ = probe_files
    for value in ("900", "1200"):
        monkeypatch.setenv(DOWNLOAD_TIMEOUT_ENV, value)
        probe.download(image)
    assert [kwargs["timeout"] for _, kwargs in runs] == [900.0, 1200.0]


def test_download_environment_does_not_change_other_deadlines(probe_files, runs, monkeypatch):
    probe, _, _ = probe_files
    monkeypatch.setenv(DOWNLOAD_TIMEOUT_ENV, "malformed")
    probe.reset()
    assert probe.read_words(0x20001000, 1) == [0x12345678]
    probe.write_words(0x20001000, [0x12345678])
    assert [(argv[1], kwargs["timeout"]) for argv, kwargs in runs] == [
        ("reset", RESET_TIMEOUT_S),
        ("read", ACCESS_TIMEOUT_S),
        ("write", ACCESS_TIMEOUT_S),
    ]
    assert RESET_TIMEOUT_S == ACCESS_TIMEOUT_S == 30.0


@pytest.mark.parametrize("value,expected", [("600", 600.0), (" 0.5 ", 0.5)])
def test_reset_environment_deadline(probe_files, runs, monkeypatch, value, expected):
    probe, _, _ = probe_files
    monkeypatch.setenv(RESET_TIMEOUT_ENV, value)
    probe.reset()
    assert runs[0][1]["timeout"] == expected


@pytest.mark.parametrize("value", ["", "nope", "0", "-1", "nan", "inf"])
def test_reset_rejects_invalid_environment_deadline(probe_files, runs, monkeypatch, value):
    probe, _, _ = probe_files
    monkeypatch.setenv(RESET_TIMEOUT_ENV, value)
    with pytest.raises(ProbeError, match="invalid reset timeout .* expected positive finite seconds"):
        probe.reset()
    assert not runs


def test_explicit_reset_deadline_has_priority(probe_files, runs, monkeypatch):
    probe, _, _ = probe_files
    monkeypatch.setenv(RESET_TIMEOUT_ENV, "malformed")
    probe.reset(timeout=2.5)
    assert runs[0][1]["timeout"] == 2.5


def test_reset_environment_does_not_change_other_deadlines(probe_files, runs, monkeypatch):
    probe, image, _ = probe_files
    monkeypatch.setenv(RESET_TIMEOUT_ENV, "malformed")
    probe.download(image)
    probe.read_words(0x20001000, 1)
    assert [(argv[1], kwargs["timeout"]) for argv, kwargs in runs] == [
        ("download", DOWNLOAD_TIMEOUT_S),
        ("read", ACCESS_TIMEOUT_S),
    ]


@pytest.mark.parametrize("value,expected", [("600", 600.0), (" 0.5 ", 0.5)])
def test_access_environment_deadline(probe_files, runs, monkeypatch, value, expected):
    probe, _, _ = probe_files
    monkeypatch.setenv(ACCESS_TIMEOUT_ENV, value)
    probe.read_words(0x20001000, 1)
    probe.write_words(0x20001000, [0x12345678])
    assert [(argv[1], kwargs["timeout"]) for argv, kwargs in runs] == [("read", expected), ("write", expected)]


@pytest.mark.parametrize("value", ["", "nope", "0", "-1", "nan", "inf"])
def test_access_rejects_invalid_environment_deadline(probe_files, runs, monkeypatch, value):
    probe, _, _ = probe_files
    monkeypatch.setenv(ACCESS_TIMEOUT_ENV, value)
    with pytest.raises(ProbeError, match="invalid access timeout .* expected positive finite seconds"):
        probe.read_words(0x20001000, 1)
    with pytest.raises(ProbeError, match="invalid access timeout .* expected positive finite seconds"):
        probe.write_words(0x20001000, [0x12345678])
    assert not runs


def test_explicit_access_deadline_has_priority(probe_files, runs, monkeypatch):
    probe, _, _ = probe_files
    monkeypatch.setenv(ACCESS_TIMEOUT_ENV, "malformed")
    probe.read_words(0x20001000, 1, timeout=2.5)
    probe.write_words(0x20001000, [0x12345678], timeout=3.5)
    assert [kwargs["timeout"] for _, kwargs in runs] == [2.5, 3.5]


def test_access_environment_does_not_change_other_deadlines(probe_files, runs, monkeypatch):
    probe, image, _ = probe_files
    monkeypatch.setenv(ACCESS_TIMEOUT_ENV, "malformed")
    probe.download(image)
    probe.reset()
    assert [(argv[1], kwargs["timeout"]) for argv, kwargs in runs] == [
        ("download", DOWNLOAD_TIMEOUT_S),
        ("reset", RESET_TIMEOUT_S),
    ]


@pytest.mark.parametrize("suffix", [".elf", ".HEX"])
@pytest.mark.parametrize("verify,reset", [(True, True), (True, False), (False, True), (False, False)])
def test_download_preserves_arguments(probe_files, runs, monkeypatch, suffix, verify, reset):
    probe, original, _ = probe_files
    image = original.with_suffix(suffix)
    if image != original:
        image.write_bytes(original.read_bytes())
    monkeypatch.setenv(DOWNLOAD_TIMEOUT_ENV, "900")
    probe.download(image, verify=verify, reset=reset)
    expected = [
        probe.executable, "download", "--chip-description-path", str(probe.chip_description),
        "--chip", "RP2350_Core0", "--probe", "test-probe-id", "--protocol", "swd", "--speed", "1000",
    ]
    if suffix.lower() == ".hex":
        expected += ["--binary-format", "hex"]
    if verify:
        expected.append("--verify")
    if reset:
        expected.append("--reset")
    expected.append(str(image))
    assert len(runs) == 1
    assert runs[0][0] == expected
    assert runs[0][1]["timeout"] == 900.0
    assert runs[0][1]["capture_output"] is True
    assert runs[0][1]["text"] is True
    assert runs[0][1]["check"] is False


def fake_executable(path, source):
    path.write_text(f"#!{sys.executable}\n" + source)
    path.chmod(0o755)


def test_download_enforces_real_subprocess_deadline(probe_files, monkeypatch):
    probe, image, executable = probe_files
    fake_executable(executable, "import time\ntime.sleep(5)\n")
    monkeypatch.setenv(DOWNLOAD_TIMEOUT_ENV, "0.1")
    with pytest.raises(ProbeError, match="timed out after 0.1 s") as caught:
        probe.download(image)
    assert isinstance(caught.value.__cause__, subprocess.TimeoutExpired)
    assert caught.value.__cause__.timeout == 0.1
    assert caught.value.argv[0] == str(executable)
    assert caught.value.argv[-1] == str(image)


def test_a_timed_out_download_keeps_what_probe_rs_printed(probe_files, monkeypatch):
    probe, image, executable = probe_files
    fake_executable(executable, "import sys, time\nprint('Erasing', flush=True)\n"
                    "print('page write failed', file=sys.stderr, flush=True)\ntime.sleep(5)\n")
    monkeypatch.setenv(DOWNLOAD_TIMEOUT_ENV, "0.5")
    with pytest.raises(ProbeError, match="timed out after 0.5 s: page write failed") as caught:
        probe.download(image)
    assert caught.value.stdout.strip() == "Erasing"
    assert caught.value.stderr.strip() == "page write failed"


def test_fake_subprocess_receives_isolated_paths(probe_files):
    probe, image, executable = probe_files
    names = ["HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME", "TMPDIR"]
    fake_executable(executable, f"import json, os\nprint(json.dumps({{name: os.environ[name] for name in {names!r}}}))\n")
    assert json.loads(probe.download(image)) == {name: os.environ[name] for name in names}


def test_download_preserves_nonzero_exit_details(probe_files, monkeypatch):
    probe, image, executable = probe_files
    fake_executable(executable, "import sys\nprint('download stdout')\nprint('download stderr', file=sys.stderr)\nsys.exit(23)\n")
    monkeypatch.setenv(DOWNLOAD_TIMEOUT_ENV, "900")
    with pytest.raises(ProbeError, match="exited 23: download stderr") as caught:
        probe.download(image)
    assert caught.value.returncode == 23
    assert caught.value.stdout == "download stdout\n"
    assert caught.value.stderr == "download stderr\n"
    assert caught.value.argv[0] == str(executable)


def test_download_preserves_missing_executable_error(probe_files):
    probe, image, _ = probe_files
    with pytest.raises(ProbeError, match="cannot run") as caught:
        probe.download(image)
    assert isinstance(caught.value.__cause__, FileNotFoundError)
    assert caught.value.argv[0] == probe.executable


def test_a_timed_out_run_calls_on_failure_before_raising(probe_files, monkeypatch):
    _, _, executable = probe_files
    calls = []
    probe = ProbeRs("test-probe-id", None, executable=executable, on_failure=lambda: calls.append("recover"))
    fake_executable(executable, "import time\ntime.sleep(5)\n")
    monkeypatch.setenv(ACCESS_TIMEOUT_ENV, "0.1")
    with pytest.raises(ProbeError, match="timed out after 0.1 s"):
        probe.read_words(0x20001000, 1)
    assert calls == ["recover"]


def test_a_failed_run_calls_on_failure_before_raising(probe_files):
    _, _, executable = probe_files
    calls = []
    probe = ProbeRs("test-probe-id", None, executable=executable, on_failure=lambda: calls.append("recover"))
    fake_executable(executable, "import sys\nsys.exit(1)\n")
    with pytest.raises(ProbeError, match="exited 1"):
        probe.reset()
    assert calls == ["recover"]


def test_a_successful_run_does_not_call_on_failure(probe_files, runs):
    _, image, executable = probe_files
    calls = []
    probe = ProbeRs("test-probe-id", None, executable=executable, on_failure=lambda: calls.append("recover"))
    probe.download(image)
    probe.reset()
    probe.read_words(0x20001000, 1)
    assert calls == []


def test_a_failing_recovery_keeps_the_probe_error(probe_files):
    _, _, executable = probe_files

    def recover():
        raise OSError("no harness")

    probe = ProbeRs("test-probe-id", None, executable=executable, on_failure=recover)
    fake_executable(executable, "import sys\nsys.exit(2)\n")
    with pytest.raises(ProbeError, match="exited 2") as caught:
        probe.reset()
    assert any("no harness" in note for note in caught.value.__notes__)
