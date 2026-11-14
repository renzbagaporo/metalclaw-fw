# SPDX-License-Identifier: Apache-2.0 OR MIT
# The host's load in the session header and beside every failure, so a failure
# on a busy or oversubscribed host can be told from one on an idle host.

import os
from pathlib import Path

import pytest

STAT = Path("/proc/stat")


# The aggregate CPU line's user, nice, system, idle, iowait, irq, softirq and
# steal times, or None where the host has no /proc/stat.
def cpu_times(stat: Path = STAT) -> list[int] | None:
    try:
        fields = stat.read_text().split("\n", 1)[0].split()
    except OSError:
        return None
    return [int(field) for field in fields[1:9]]


def describe(before: list[int] | None = None, after: list[int] | None = None) -> str:
    load1, load5, load15 = os.getloadavg()
    text = f"load {load1:.1f} {load5:.1f} {load15:.1f} on {os.cpu_count()} CPUs"
    if before is None or after is None:
        return text
    user, nice, system, idle, iowait, irq, softirq, steal = (b - a for a, b in zip(before, after))
    total = user + nice + system + idle + iowait + irq + softirq + steal
    if total <= 0:
        return text
    busy = user + nice + system + irq + softirq
    return f"{text}; during the test {100 * busy // total}% busy, {100 * steal // total}% stolen by the hypervisor"


def pytest_report_header() -> str:
    return "host: " + describe()


_started: dict[str, list[int] | None] = {}


def pytest_runtest_logstart(nodeid: str) -> None:
    _started[nodeid] = cpu_times()


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item):
    report = yield
    if report.failed:
        report.sections.append(("host load", describe(_started.get(item.nodeid), cpu_times())))
    return report
