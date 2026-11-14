# SPDX-License-Identifier: Apache-2.0 OR MIT

from test.rig import host_load

pytest_plugins = ["pytester"]


def test_cpu_times_reads_the_aggregate_line(tmp_path):
    stat = tmp_path / "stat"
    stat.write_text("cpu  10 1 5 80 2 1 1 3 0 0\ncpu0 5 0 2 40 1 0 0 1 0 0\n")
    assert host_load.cpu_times(stat) == [10, 1, 5, 80, 2, 1, 1, 3]


def test_cpu_times_is_none_without_proc_stat(tmp_path):
    assert host_load.cpu_times(tmp_path / "missing") is None


def test_the_description_splits_busy_from_stolen_time(monkeypatch):
    monkeypatch.setattr(host_load.os, "getloadavg", lambda: (21.07, 24.3, 28.5))
    monkeypatch.setattr(host_load.os, "cpu_count", lambda: 16)
    before = [0] * 8
    after = [50, 0, 10, 20, 5, 0, 5, 10]
    assert host_load.describe(before, after) == (
        "load 21.1 24.3 28.5 on 16 CPUs; during the test 65% busy, 10% stolen by the hypervisor")


def test_the_description_is_the_load_alone_without_cpu_times(monkeypatch):
    monkeypatch.setattr(host_load.os, "getloadavg", lambda: (1.0, 2.0, 3.0))
    monkeypatch.setattr(host_load.os, "cpu_count", lambda: 4)
    assert host_load.describe(None, [0] * 8) == "load 1.0 2.0 3.0 on 4 CPUs"


def test_a_failure_carries_the_host_load_and_a_pass_does_not(pytester):
    pytester.makepyfile("def test_fails(): assert False\ndef test_passes(): pass\n")
    result = pytester.runpytest("-p", "test.rig.host_load", "-rA")
    result.stdout.fnmatch_lines(["host: load * on * CPUs", "*- host load -*", "load * on * CPUs; during the test *"])
    assert result.stdout.str().count("during the test") == 1
