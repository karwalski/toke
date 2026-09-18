"""pytest for count_tokens.py region isolation (story 131.4). Needs ./tkc."""
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import count_tokens as ct  # noqa: E402

pytestmark = pytest.mark.skipif(not ct.TKC.exists(), reason="tkc binary not built")

PROG = ('m=main;\ni=io:std.io;\nf=pat(x:i64):str{\n  let s="héllo \\(x) w}rld";\n  <s\n};\n'
        'f=main():i64{\n  io.println(pat(1));\n  <0\n};\n')


def test_function_extent_excludes_harness_and_handles_utf8_and_brace_in_string():
    m = ct.min_text_of(PROG)
    s, e = ct.function_extent(m, "pat")
    assert m[s:e] == 'f=pat(x:i64):str{let s="héllo \\(x) w}rld";<s}'
    s2, e2 = ct.function_extent(m, "main")
    assert m[s2:e2] == 'f=main():i64{io.println(pat(1));<0}'


def test_missing_function_raises():
    m = ct.min_text_of(PROG)
    with pytest.raises(RuntimeError):
        ct.function_extent(m, "nope")
