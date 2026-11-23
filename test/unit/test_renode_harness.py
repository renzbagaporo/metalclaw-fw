# SPDX-License-Identifier: Apache-2.0 OR MIT

import inspect

import pytest

from test.rig.renode import harness


@pytest.mark.parametrize("wait", ["serial_port", "wait_interface", "wait_interfaces"])
def test_the_emulated_harness_waits_on_usb_as_long_as_for_enumeration(wait):
    default = inspect.signature(getattr(harness.EmulatedHarness, wait)).parameters["timeout"].default
    assert default == harness.ENUMERATION_TIMEOUT_S


def test_the_time_scale_comes_from_the_environment(monkeypatch):
    monkeypatch.setenv(harness.TIME_SCALE_ENV, "2.5")
    assert harness.time_scale() == 2.5
    monkeypatch.delenv(harness.TIME_SCALE_ENV)
    assert harness.time_scale() == harness.TIME_SCALE


@pytest.mark.parametrize("value", ["0", "-1", "nan", "inf", "fast"])
def test_an_invalid_time_scale_is_refused(monkeypatch, value):
    monkeypatch.setenv(harness.TIME_SCALE_ENV, value)
    with pytest.raises(ValueError, match="positive finite factor"):
        harness.time_scale()


def test_the_emulated_harness_scales_repl_deadlines(monkeypatch):
    seen = []
    monkeypatch.setattr(harness.Harness, "run", lambda self, code, timeout=5.0: seen.append(timeout) or "")
    monkeypatch.setenv(harness.TIME_SCALE_ENV, "4")
    harness.EmulatedHarness("0123456789ABCDEF").run("pass", 2.5)
    assert seen == [10.0]
