# SPDX-License-Identifier: Apache-2.0 OR MIT
# Spec's per-line matching, as Zephyr's twister console harness matches.

from test.pico_examples.targets import Spec


def test_a_line_pattern_anchors_to_its_line():
    spec = Spec(console=(r"^ready$",), lines=True)
    assert spec.match("boot\r\nready\r\n")[0] == []
    assert spec.match("boot\r\nnot ready\r\n")[0] == [r"^ready$"]


def test_ordered_lines_need_a_later_line_each():
    spec = Spec(console=("a", "b"), lines=True)
    assert spec.match("a\nb\n")[0] == []
    assert spec.match("ab\n")[0] == ["b"]
    assert spec.match("b\na\n")[0] == ["b"]


def test_unordered_lines_match_anywhere():
    spec = Spec(console=("b", "a"), lines=True, ordered=False)
    assert spec.match("a\nb\n")[0] == []
    assert spec.match("ab\n")[0] == []
    assert spec.match("a\n")[0] == ["b"]


def test_a_line_match_ends_where_its_line_ends():
    spec = Spec(console=("two",), lines=True)
    text = "one\r\ntwo\r\nthree"
    assert spec.match_ends(text) == [text.index("three")]


def test_an_unfinished_last_line_counts():
    spec = Spec(console=(r"uart:~\$ $",), lines=True)
    assert spec.match("boot\nuart:~$ ")[0] == []
